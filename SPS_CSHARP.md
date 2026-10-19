# SPS in C#

[csharp/Sps.cs](csharp/Sps.cs) implements the SPS credit-based flow control for both roles in portable C# (`netstandard2.0`: .NET Framework 4.6.1 and later, .NET 5 and later, Unity, MAUI). It is the C# counterpart of `sps.c` and has no BLE stack: implement `ISpsTransport` on top of your BLE API and feed `SpsLink` the GATT events.

| File | Content |
|------|---------|
| [csharp/Sps.cs](csharp/Sps.cs) | `SpsLink`, `ISpsTransport` and the 128-bit UUIDs |
| [csharp/Sps.csproj](csharp/Sps.csproj) | Library project, assembly `Ublox.Sps` |
| [csharp/Sps.Tests](csharp/Sps.Tests) | Host test without packages: a client and a server wired together in memory, the same cases as `test_sps.c` |

Build and test on a PC:

```bash
dotnet run --project csharp/Sps.Tests
```

Related documentation: [SPS_PROTOCOL.md](SPS_PROTOCOL.md), [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md), [C implementation](SPS_CLIENT_C.md).

## Connect it to your BLE API

| `ISpsTransport` | Client | Server |
|-----------------|--------|--------|
| `SendFifo` | Write FIFO without response | Notify FIFO |
| `SendCredits` | Write Credits (1 byte) | Notify Credits (1 byte) |
| `Subscribe` | Write `0x0001` to the CCCD of the FIFO or Credits characteristic | Not used |

| BLE event | Call |
|-----------|------|
| Connected and, for the client, the SPS service is discovered | `OnConnected()` |
| FIFO notification (client) or FIFO write (server) | `OnFifo()` |
| Credits notification (client) or Credits write (server) | `OnCredits()` |
| Client enabled the Credits notifications (server) | `OnCreditsSubscribed()` |
| Disconnected | `OnDisconnected()` |
| Still `SpsState.Connected` a few seconds after `OnConnected()` (client) | `RetryHandshake()` once |

The transport methods return `true` when the stack accepted the request. BLE APIs are asynchronous: queue the calls in order inside the adapter and do not block waiting for a GATT event. `SpsLink` is not thread safe, call it from one thread or serialize the calls.

`OnConnected()` follows UBX-16011192 figure 5: Credits notifications first, FIFO notifications second, then the initial credits. With flow control disabled only the FIFO notifications are enabled. Flow control must be set the same on both sides, see [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md).

`Send()` queues the data and sends what the credits allow. It returns the number of bytes queued, which is less than requested when the queue (1024 bytes by default) is full. Call it again with the rest later, see `TxFree`. Set `PacketSize` to the ATT MTU minus 3 (default 244). `DataReceived` is raised for each packet and the packet is credited back after the handler returns.

## Example

```csharp
using System;
using System.Text;
using Ublox.Sps;

sealed class BleTransport : ISpsTransport
{
    private readonly IMyBle _ble; // your BLE API

    public BleTransport(IMyBle ble) => _ble = ble;

    public bool SendFifo(byte[] packet) => _ble.WriteWithoutResponse(SpsUuids.Fifo, packet);

    public bool SendCredits(byte credits) => _ble.WriteWithoutResponse(SpsUuids.Credits, new[] { credits });

    public bool Subscribe(SpsCharacteristic c) =>
        _ble.EnableNotifications(c == SpsCharacteristic.Fifo ? SpsUuids.Fifo : SpsUuids.Credits);
}

var link = new SpsLink(SpsRole.Client, new BleTransport(ble));
link.DataReceived += data => Console.Write(Encoding.UTF8.GetString(data));
ble.ServiceDiscovered += () => link.OnConnected();
ble.Disconnected += () => link.OnDisconnected();
ble.Notified += (uuid, data) =>
{
    if (uuid == SpsUuids.Fifo) link.OnFifo(data);
    else if (uuid == SpsUuids.Credits) link.OnCredits(data);
};

link.Send(Encoding.UTF8.GetBytes("Hello"));
```

## Platform functions

| API | Write FIFO and Credits | Enable notifications | Receive notifications |
|-----|------------------------|----------------------|-----------------------|
| WinRT (`Windows.Devices.Bluetooth.GenericAttributeProfile`) | `GattCharacteristic.WriteValueAsync()` with `GattWriteOption.WriteWithoutResponse` | `WriteClientCharacteristicConfigurationDescriptorAsync()` with `Notify` | `GattCharacteristic.ValueChanged` |
| Plugin.BLE | `ICharacteristic.WriteAsync()` with `WriteType = WithoutResponse` | `StartUpdatesAsync()` | `ValueUpdated` |

For the server role on Windows use `GattServiceProvider`: `GattLocalCharacteristic.NotifyValueAsync()` to notify, `WriteRequested` for writes and `SubscribedClientsChanged` for CCCD changes.
