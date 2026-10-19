// SPDX-License-Identifier: Apache-2.0
//
// u-blox Serial Port Service (SPS): credit-based flow control for both roles, in portable C#.
// The same engine as sps.c, without a BLE stack. Implement ISpsTransport on top of your BLE API
// (WinRT, Plugin.BLE, ...) and feed SpsLink the GATT events:
//
//                       CLIENT (central)                SERVER (peripheral)
//   SendFifo            write FIFO                      notify FIFO
//   SendCredits         write Credits                   notify Credits
//   Subscribe           write the CCCD of the char      not used
//   OnFifo              FIFO notification received      FIFO write received
//   OnCredits           Credits notification received   Credits write received
//
// SpsLink is not thread safe: call it from one thread or serialize the calls. The transport may
// call back into SpsLink synchronously, Flush() is re-entrancy safe. Do not block inside a
// transport method waiting for a GATT event.

#nullable enable

using System;

namespace Ublox.Sps
{
    public enum SpsRole
    {
        /// <summary>BLE central, GATT client.</summary>
        Client,

        /// <summary>BLE peripheral, GATT server.</summary>
        Server,
    }

    public enum SpsState
    {
        Disconnected,

        /// <summary>Link up, waiting for the first credits.</summary>
        Connected,

        /// <summary>Data can flow.</summary>
        Ready,
    }

    public enum SpsCharacteristic
    {
        Fifo,
        Credits,
    }

    /// <summary>128-bit UUIDs of the service and its characteristics.</summary>
    public static class SpsUuids
    {
        public static readonly Guid Service = new Guid("2456e1b9-26e2-8f83-e744-f34f01e9d701");
        public static readonly Guid Fifo = new Guid("2456e1b9-26e2-8f83-e744-f34f01e9d703");
        public static readonly Guid Credits = new Guid("2456e1b9-26e2-8f83-e744-f34f01e9d704");
    }

    /// <summary>The BLE side of SpsLink. Each method returns true when the stack accepted the request.</summary>
    public interface ISpsTransport
    {
        /// <summary>One packet on the FIFO characteristic. The array belongs to the transport.</summary>
        bool SendFifo(byte[] packet);

        /// <summary>One credit count (1..255) on the Credits characteristic. Required with flow control.</summary>
        bool SendCredits(byte credits);

        /// <summary>Client only: enable the notifications of one characteristic (write its CCCD).</summary>
        bool Subscribe(SpsCharacteristic characteristic);
    }

    public sealed class SpsStats
    {
        public long BytesSent { get; internal set; }
        public long BytesReceived { get; internal set; }
        public long PacketsSent { get; internal set; }
        public long PacketsReceived { get; internal set; }
        public long CreditsSent { get; internal set; }
        public long CreditsReceived { get; internal set; }
    }

    public sealed class SpsLink
    {
        /// <summary>Same value as the VERSION file: the u-connectXpress release it is checked against.</summary>
        public const string Version = "3.5.0";

        public const int DefaultPacketSize = 244; // ATT MTU 247 - 3 byte header
        public const int InitialCredits = 8;      // credits granted when the link is ready
        public const int CreditThreshold = 4;     // grant credits after this many packets
        public const int DefaultTxBufferSize = 1024;

        private readonly ISpsTransport _transport;
        private readonly byte[] _tx;
        private int _txLength;
        private int _rxPending;
        private int _packetSize = DefaultPacketSize;
        private bool _initialGranted;
        private bool _flushing;

        /// <summary>
        /// Flow control must be set the same on both sides. It is not negotiated (on u-blox modules:
        /// AT+UDSC parameter 4).
        /// </summary>
        /// <param name="txBufferSize">Bytes Send() can queue while it waits for credits.</param>
        public SpsLink(SpsRole role, ISpsTransport transport, bool flowControl = true,
                       int txBufferSize = DefaultTxBufferSize)
        {
            if (transport == null)
            {
                throw new ArgumentNullException(nameof(transport));
            }
            if (txBufferSize < 1)
            {
                throw new ArgumentOutOfRangeException(nameof(txBufferSize));
            }
            Role = role;
            FlowControl = flowControl;
            _transport = transport;
            _tx = new byte[txBufferSize];
        }

        public SpsRole Role { get; }
        public bool FlowControl { get; }
        public SpsState State { get; private set; } = SpsState.Disconnected;
        public SpsStats Stats { get; } = new SpsStats();

        /// <summary>Packets that may still be sent.</summary>
        public int TxCredits { get; private set; }

        /// <summary>Free bytes in the transmit queue.</summary>
        public int TxFree => _tx.Length - _txLength;

        /// <summary>Payload per packet, 1..txBufferSize. Use ATT MTU - 3.</summary>
        public int PacketSize
        {
            get => _packetSize;
            set
            {
                if (value < 1 || value > _tx.Length)
                {
                    throw new ArgumentOutOfRangeException(nameof(value));
                }
                _packetSize = value;
            }
        }

        /// <summary>Data received. The packet is credited back after the handler returns.</summary>
        public event Action<byte[]>? DataReceived;

        public event Action<SpsState>? StateChanged;

        /// <summary>
        /// The BLE link is up (service discovered, for the client). The client enables the Credits
        /// notifications first and the FIFO notifications second, then grants InitialCredits, as
        /// UBX-16011192 requires. Returns false when the transport failed: the link is then back in
        /// Disconnected and the caller must drop it.
        /// </summary>
        public bool OnConnected()
        {
            if (State != SpsState.Disconnected)
            {
                throw new InvalidOperationException("The link is already connected.");
            }
            ResetLink();
            SetState(SpsState.Connected);

            if (Role == SpsRole.Server)
            {
                // With flow control the initial credits follow the Credits subscription.
                if (!FlowControl)
                {
                    SetState(SpsState.Ready);
                }
                return true;
            }

            if (!FlowControl)
            {
                // No Credits characteristic traffic at all, some firmware drops the link otherwise.
                if (!_transport.Subscribe(SpsCharacteristic.Fifo))
                {
                    return FailConnect();
                }
                SetState(SpsState.Ready);
                return true;
            }

            // UBX-16011192 figure 5: Credits CCCD first, FIFO CCCD second, then the credits.
            if (!_transport.Subscribe(SpsCharacteristic.Credits) ||
                !_transport.Subscribe(SpsCharacteristic.Fifo) ||
                !Grant(InitialCredits))
            {
                return FailConnect();
            }
            _initialGranted = true;
            return true;
        }

        /// <summary>The BLE link is gone. Clears credits and queued data.</summary>
        public void OnDisconnected()
        {
            ResetLink();
            SetState(SpsState.Disconnected);
        }

        /// <summary>
        /// Client: call once if the state is still Connected a few seconds after OnConnected(), for
        /// example when a first grant was lost while pairing. Returns true when a retry was sent.
        /// </summary>
        public bool RetryHandshake()
        {
            if (Role != SpsRole.Client || !FlowControl || State != SpsState.Connected)
            {
                return false;
            }
            return Grant(InitialCredits);
        }

        /// <summary>Server: the client enabled the Credits notifications (CCCD write). Grants the initial credits.</summary>
        public void OnCreditsSubscribed()
        {
            if (Role == SpsRole.Server && State != SpsState.Disconnected)
            {
                GrantInitial();
            }
        }

        /// <summary>A FIFO notification (client) or FIFO write (server) arrived.</summary>
        public void OnFifo(byte[] data)
        {
            if (data == null || data.Length == 0 || State == SpsState.Disconnected)
            {
                return;
            }
            Stats.BytesReceived += data.Length;
            Stats.PacketsReceived++;
            DataReceived?.Invoke(data);
            if (!FlowControl)
            {
                return;
            }
            // The first packet proves the client is subscribed even if the grant at subscription was lost.
            if (Role == SpsRole.Server)
            {
                GrantInitial();
            }
            if (_rxPending < byte.MaxValue)
            {
                _rxPending++;
            }
            if (_rxPending >= CreditThreshold && Grant((byte)_rxPending))
            {
                _rxPending = 0;
            }
        }

        /// <summary>A Credits notification (client) or Credits write (server) arrived.</summary>
        public void OnCredits(byte[] data)
        {
            if (data == null || data.Length == 0 || !FlowControl || State == SpsState.Disconnected)
            {
                return;
            }
            int credits = data[0];
            Stats.CreditsReceived += credits;
            TxCredits = Math.Min(TxCredits + credits, ushort.MaxValue);

            // Credits written by the client mean it is subscribed and waits for ours.
            if (Role == SpsRole.Server)
            {
                GrantInitial();
            }
            if (State == SpsState.Connected)
            {
                SetState(SpsState.Ready);
            }
            Flush();
        }

        public int Send(byte[] data)
        {
            if (data == null)
            {
                throw new ArgumentNullException(nameof(data));
            }
            return Send(data, 0, data.Length);
        }

        /// <summary>
        /// Queue data and send what the credits allow. Returns the number of bytes queued: less than
        /// count when the queue is full, 0 when it is full. Send the rest later, see TxFree.
        /// </summary>
        public int Send(byte[] data, int offset, int count)
        {
            if (data == null)
            {
                throw new ArgumentNullException(nameof(data));
            }
            if (offset < 0 || count < 0 || offset > data.Length - count)
            {
                throw new ArgumentOutOfRangeException(nameof(count));
            }
            if (State == SpsState.Disconnected)
            {
                throw new InvalidOperationException("The link is not connected.");
            }
            int n = Math.Min(count, TxFree);
            Buffer.BlockCopy(data, offset, _tx, _txLength, n);
            _txLength += n;
            Flush();
            return n;
        }

        /// <summary>Send queued data if the state and credits allow. Called by Send() and OnCredits().</summary>
        public void Flush()
        {
            if (State != SpsState.Ready || _flushing)
            {
                return;
            }
            _flushing = true;
            while (_txLength > 0 && (!FlowControl || TxCredits > 0))
            {
                int n = Math.Min(_txLength, _packetSize);
                byte[] packet = new byte[n];
                Buffer.BlockCopy(_tx, 0, packet, 0, n);
                if (!_transport.SendFifo(packet))
                {
                    break;
                }
                // The transport may have dropped the link.
                if (State != SpsState.Ready || _txLength < n)
                {
                    break;
                }
                if (FlowControl)
                {
                    TxCredits--;
                }
                Stats.BytesSent += n;
                Stats.PacketsSent++;
                // _txLength may have grown inside the callback; the first n bytes are ours.
                Buffer.BlockCopy(_tx, n, _tx, 0, _txLength - n);
                _txLength -= n;
            }
            _flushing = false;
        }

        private bool Grant(byte count)
        {
            if (count == 0)
            {
                return true;
            }
            if (!_transport.SendCredits(count))
            {
                return false;
            }
            Stats.CreditsSent += count;
            return true;
        }

        private void GrantInitial()
        {
            if (FlowControl && !_initialGranted && Grant(InitialCredits))
            {
                _initialGranted = true;
            }
        }

        private bool FailConnect()
        {
            OnDisconnected();
            return false;
        }

        private void ResetLink()
        {
            _initialGranted = false;
            _flushing = false;
            _rxPending = 0;
            TxCredits = 0;
            _txLength = 0;
        }

        private void SetState(SpsState state)
        {
            if (State == state)
            {
                return;
            }
            State = state;
            StateChanged?.Invoke(state);
        }
    }
}
