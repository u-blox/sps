// SPDX-License-Identifier: Apache-2.0
//
// Host test for Sps.cs: a client and a server are wired together through an in-memory link, either
// queued (like a BLE stack that calls back from its own thread) or synchronous. Same cases as
// test_sps.c.
//
//   dotnet run --project csharp/Sps.Tests

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Ublox.Sps;

namespace Ublox.Sps.Tests
{
    internal sealed class Node : ISpsTransport
    {
        public SpsLink Link = null!;
        public Node Peer = null!;
        public readonly List<byte> Received = new List<byte>();
        public readonly List<SpsCharacteristic> Subscriptions = new List<SpsCharacteristic>();
        public readonly List<SpsState> States = new List<SpsState>();
        public bool FailFifo;      // SendFifo returns false
        public int DropCredits;    // silently lose this many outgoing credit values
        public int CreditsOut;     // SendCredits calls

        public bool SendFifo(byte[] packet)
        {
            if (FailFifo)
            {
                return false;
            }
            Peer.Post(() => Peer.Link.OnFifo(packet));
            return true;
        }

        public bool SendCredits(byte credits)
        {
            CreditsOut++;
            if (DropCredits > 0)
            {
                DropCredits--;
                return true;
            }
            Peer.Post(() => Peer.Link.OnCredits(new[] { credits }));
            return true;
        }

        public bool Subscribe(SpsCharacteristic characteristic)
        {
            Subscriptions.Add(characteristic);
            if (characteristic == SpsCharacteristic.Credits)
            {
                Peer.Post(() => Peer.Link.OnCreditsSubscribed());
            }
            return true;
        }

        public void Post(Action action) => Program.Post(action);
    }

    internal static class Program
    {
        private static int _checks;
        private static int _failed;
        private static bool _sync;
        private static readonly Queue<Action> Events = new Queue<Action>();
        private static Node _client = null!;
        private static Node _server = null!;

        public static void Post(Action action)
        {
            if (_sync)
            {
                action();
            }
            else
            {
                Events.Enqueue(action);
            }
        }

        private static void Pump()
        {
            while (Events.Count > 0)
            {
                Events.Dequeue()();
            }
        }

        private static void Check(bool condition,
                                  [CallerArgumentExpression("condition")] string expression = "",
                                  [CallerLineNumber] int line = 0)
        {
            _checks++;
            if (!condition)
            {
                _failed++;
                Console.WriteLine($"FAIL Program.cs:{line}: {expression}");
            }
        }

        private static bool Throws<T>(Action action) where T : Exception
        {
            try
            {
                action();
            }
            catch (T)
            {
                return true;
            }
            return false;
        }

        private static void Setup(bool flowControl, bool sync = false)
        {
            Events.Clear();
            _sync = sync;
            _client = new Node();
            _server = new Node();
            _client.Peer = _server;
            _server.Peer = _client;
            foreach (Node node in new[] { _client, _server })
            {
                SpsRole role = node == _client ? SpsRole.Client : SpsRole.Server;
                node.Link = new SpsLink(role, node, flowControl);
                Node captured = node;
                node.Link.DataReceived += data => captured.Received.AddRange(data);
                node.Link.StateChanged += state => captured.States.Add(state);
            }
        }

        private static void ConnectBoth()
        {
            Check(_server.Link.OnConnected());
            Check(_client.Link.OnConnected());
            Pump();
        }

        private static byte PatternByte(int i, int seed) => (byte)(i * 7 + seed);

        private static byte[] PatternBytes(int length, int seed)
        {
            byte[] data = new byte[length];
            for (int i = 0; i < length; i++)
            {
                data[i] = PatternByte(i, seed);
            }
            return data;
        }

        /// <summary>Both sides send `total` bytes at the same time. Returns once everything arrived.</summary>
        private static void TransferBoth(int total)
        {
            byte[][] source = { PatternBytes(total, 1), PatternBytes(total, 2) };
            Node[] nodes = { _client, _server };
            int[] offset = { 0, 0 };

            for (int iteration = 0; iteration < 1000; iteration++)
            {
                for (int side = 0; side < 2; side++)
                {
                    int n;
                    while (offset[side] < total &&
                           (n = nodes[side].Link.Send(source[side], offset[side], total - offset[side])) > 0)
                    {
                        offset[side] += n;
                    }
                }
                Pump();
                if (offset[0] == total && offset[1] == total && Events.Count == 0 &&
                    _client.Link.TxFree == SpsLink.DefaultTxBufferSize &&
                    _server.Link.TxFree == SpsLink.DefaultTxBufferSize)
                {
                    break;
                }
            }
            Check(offset[0] == total && offset[1] == total);
            Check(Same(_server.Received, source[0]));
            Check(Same(_client.Received, source[1]));
        }

        private static bool Same(List<byte> received, byte[] expected)
        {
            if (received.Count != expected.Length)
            {
                return false;
            }
            for (int i = 0; i < expected.Length; i++)
            {
                if (received[i] != expected[i])
                {
                    return false;
                }
            }
            return true;
        }

        private static void TestUuids()
        {
            Check(SpsUuids.Service.ToString() == "2456e1b9-26e2-8f83-e744-f34f01e9d701");
            Check(SpsUuids.Fifo.ToString() == "2456e1b9-26e2-8f83-e744-f34f01e9d703");
            Check(SpsUuids.Credits.ToString() == "2456e1b9-26e2-8f83-e744-f34f01e9d704");
        }

        private static void TestArguments()
        {
            Setup(true);
            Check(Throws<ArgumentNullException>(() => new SpsLink(SpsRole.Client, null!)));
            Check(Throws<ArgumentOutOfRangeException>(() => new SpsLink(SpsRole.Client, _client, true, 0)));
            Check(Throws<ArgumentOutOfRangeException>(() => _client.Link.PacketSize = 0));
            Check(Throws<ArgumentOutOfRangeException>(() => _client.Link.PacketSize = SpsLink.DefaultTxBufferSize + 1));
            Check(Throws<ArgumentNullException>(() => _client.Link.Send(null!)));
            Check(Throws<ArgumentOutOfRangeException>(() => _client.Link.Send(new byte[4], 3, 2)));
        }

        private static void TestHandshake()
        {
            Setup(true);
            ConnectBoth();
            Check(_client.Subscriptions.Count == 2);
            Check(_client.Subscriptions[0] == SpsCharacteristic.Credits); // UBX-16011192 figure 5
            Check(_client.Subscriptions[1] == SpsCharacteristic.Fifo);
            Check(_client.Link.State == SpsState.Ready && _server.Link.State == SpsState.Ready);
            Check(_client.Link.TxCredits == SpsLink.InitialCredits);
            Check(_server.Link.TxCredits == SpsLink.InitialCredits);
            Check(_client.Link.Stats.CreditsSent == SpsLink.InitialCredits);
            Check(_server.Link.Stats.CreditsSent == SpsLink.InitialCredits);
            Check(Throws<InvalidOperationException>(() => _client.Link.OnConnected()));
        }

        private static void TestBackpressureAndIntegrity()
        {
            Setup(true);
            ConnectBoth();
            byte[] source = PatternBytes(4096, 9);
            int offset = 0;
            int n;

            // No Pump: the client can only use the initial credits, the rest waits in the queue.
            while (offset < source.Length && (n = _client.Link.Send(source, offset, source.Length - offset)) > 0)
            {
                offset += n;
            }
            Check(_client.Link.Stats.PacketsSent == SpsLink.InitialCredits);
            Check(_client.Link.TxCredits == 0);
            Check(_client.Link.TxFree == 0);
            Check(offset == _client.Link.Stats.BytesSent + SpsLink.DefaultTxBufferSize); // sent plus a full queue

            for (int i = 0; i < 100 && (offset < source.Length || _client.Link.TxFree < SpsLink.DefaultTxBufferSize || Events.Count > 0); i++)
            {
                Pump();
                while (offset < source.Length && (n = _client.Link.Send(source, offset, source.Length - offset)) > 0)
                {
                    offset += n;
                }
            }
            Check(offset == source.Length && _client.Link.TxFree == SpsLink.DefaultTxBufferSize);
            Check(Same(_server.Received, source));
            // The sender never used a credit it did not get.
            Check(_client.Link.Stats.PacketsSent <= _client.Link.Stats.CreditsReceived);
            Check(_client.Link.TxCredits == _client.Link.Stats.CreditsReceived - _client.Link.Stats.PacketsSent);
        }

        private static void TestBidirectional()
        {
            Setup(true);
            ConnectBoth();
            TransferBoth(3000);
        }

        private static void TestSynchronousLink()
        {
            Setup(true, sync: true);
            ConnectBoth();
            Check(_client.Link.State == SpsState.Ready && _server.Link.State == SpsState.Ready);
            TransferBoth(3000);
        }

        private static void TestNoFlowControl()
        {
            Setup(false);
            ConnectBoth();
            Check(_client.Subscriptions.Count == 1 && _client.Subscriptions[0] == SpsCharacteristic.Fifo);
            Check(_client.Link.State == SpsState.Ready && _server.Link.State == SpsState.Ready);
            TransferBoth(3000);
            Check(_client.CreditsOut == 0 && _server.CreditsOut == 0);
        }

        private static void TestServerFallbacks()
        {
            // Credits written before the Credits subscription was seen.
            Setup(true);
            Check(_server.Link.OnConnected());
            Check(_server.Link.State == SpsState.Connected && _server.Link.Stats.CreditsSent == 0);
            _server.Link.OnCredits(new byte[] { 4 });
            Check(_server.Link.State == SpsState.Ready);
            Check(_server.Link.TxCredits == 4);
            Check(_server.Link.Stats.CreditsSent == SpsLink.InitialCredits);

            // First data packet before any credit exchange.
            Setup(true);
            Check(_server.Link.OnConnected());
            _server.Link.OnFifo(new byte[] { 0x55 });
            Check(_server.Link.Stats.CreditsSent == SpsLink.InitialCredits);
        }

        private static void TestCreditsReturnedAfterThreshold()
        {
            Setup(true);
            ConnectBoth();
            for (int i = 0; i < SpsLink.CreditThreshold - 1; i++)
            {
                _server.Link.OnFifo(new byte[] { 1 });
            }
            Check(_server.Link.Stats.CreditsSent == SpsLink.InitialCredits); // nothing yet
            _server.Link.OnFifo(new byte[] { 1 });
            Check(_server.Link.Stats.CreditsSent == SpsLink.InitialCredits + SpsLink.CreditThreshold);
        }

        private static void TestDisconnectResets()
        {
            Setup(true);
            ConnectBoth();
            Check(_client.Link.Send(new byte[] { 1 }) == 1);
            _client.Link.OnDisconnected();
            Check(_client.Link.State == SpsState.Disconnected);
            Check(_client.Link.TxCredits == 0 && _client.Link.TxFree == SpsLink.DefaultTxBufferSize);
            Check(Throws<InvalidOperationException>(() => _client.Link.Send(new byte[] { 1 })));
            _client.Link.OnFifo(new byte[] { 1 });
            Check(_client.Link.Stats.PacketsReceived == 0);

            Pump();
            Check(_client.Link.OnConnected()); // the link can come back
        }

        private static void TestTransportFailure()
        {
            Setup(true);
            ConnectBoth();
            byte[] data = PatternBytes(100, 3);

            _client.FailFifo = true;
            Check(_client.Link.Send(data) == data.Length);
            Check(_client.Link.Stats.PacketsSent == 0 && _client.Link.TxFree == SpsLink.DefaultTxBufferSize - 100);
            Check(_client.Link.TxCredits == SpsLink.InitialCredits); // no credit spent on a failed write

            _client.FailFifo = false;
            _client.Link.Flush();
            Check(_client.Link.Stats.PacketsSent == 1 && _client.Link.TxFree == SpsLink.DefaultTxBufferSize);
            Check(_client.Link.TxCredits == SpsLink.InitialCredits - 1);
        }

        private static void TestRetryHandshake()
        {
            Setup(true);
            _server.DropCredits = 1; // the server's first grant is lost
            ConnectBoth();
            Check(_client.Link.State == SpsState.Connected);
            Check(_client.Link.RetryHandshake());
            Check(_client.CreditsOut == 2 && _client.Link.Stats.CreditsSent == 2 * SpsLink.InitialCredits);
            Pump();

            // A server has nothing to retry, and a ready client neither.
            Check(!_server.Link.RetryHandshake());
            Setup(true);
            ConnectBoth();
            Check(!_client.Link.RetryHandshake());
        }

        private static void TestConnectFailure()
        {
            Setup(true);
            var failing = new FailingTransport();
            var link = new SpsLink(SpsRole.Client, failing);
            Check(!link.OnConnected());
            Check(link.State == SpsState.Disconnected);
        }

        private sealed class FailingTransport : ISpsTransport
        {
            public bool SendFifo(byte[] packet) => false;
            public bool SendCredits(byte credits) => false;
            public bool Subscribe(SpsCharacteristic characteristic) => false;
        }

        private static void TestPacketSize()
        {
            Setup(true);
            ConnectBoth();
            _client.Link.PacketSize = 20;
            Check(_client.Link.Send(new byte[100]) == 100);
            Check(_client.Link.Stats.PacketsSent == 5 && _client.Link.Stats.BytesSent == 100);
        }

        private static void TestCreditSaturation()
        {
            Setup(true);
            ConnectBoth();
            for (int i = 0; i < 400; i++)
            {
                _client.Link.OnCredits(new byte[] { 255 });
            }
            Check(_client.Link.TxCredits == ushort.MaxValue);
        }

        private static void TestStateEvents()
        {
            Setup(true);
            ConnectBoth();
            _client.Link.OnDisconnected();
            Check(_client.States.Count == 3);
            Check(_client.States[0] == SpsState.Connected);
            Check(_client.States[1] == SpsState.Ready);
            Check(_client.States[2] == SpsState.Disconnected);
        }

        public static int Main()
        {
            TestUuids();
            TestArguments();
            TestHandshake();
            TestBackpressureAndIntegrity();
            TestBidirectional();
            TestSynchronousLink();
            TestNoFlowControl();
            TestServerFallbacks();
            TestCreditsReturnedAfterThreshold();
            TestDisconnectResets();
            TestTransportFailure();
            TestRetryHandshake();
            TestConnectFailure();
            TestPacketSize();
            TestCreditSaturation();
            TestStateEvents();

            Console.WriteLine($"Ublox.Sps {SpsLink.Version}: {_checks} checks, {_failed} failed");
            return _failed == 0 ? 0 : 1;
        }
    }
}
