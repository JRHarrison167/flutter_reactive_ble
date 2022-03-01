#include "include/reactive_ble_windows/reactive_ble_windows_plugin.h"
#pragma comment(lib, "windowsapp")

// This must be included before many other Windows headers.
#include <windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <ppltasks.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <map>
#include <memory>
#include <sstream>
#include <iomanip>
#include <variant>

#include "../lib/src/generated/bledata.pb.h"
#include "include/reactive_ble_windows/ble_char_handler.h"
#include "include/reactive_ble_windows/ble_scan_handler.h"
#include "include/reactive_ble_windows/ble_status_handler.h"
#include "include/reactive_ble_windows/ble_utils.h"
#include "include/reactive_ble_windows/bluetooth_device_agent.h"

namespace
{
    using flutter::EncodableList;
    using flutter::EncodableMap;
    using flutter::EncodableValue;

    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Storage::Streams;


    // Obtained from reactive_ble_platform_interface, connection_state_update.dart
    enum DeviceConnectionState
    {
        connecting,
        connected,
        disconnecting,
        disconnected
    };


    /**
     * @brief Plugin to handle Windows BLE operations.
     */
    class ReactiveBleWindowsPlugin : public flutter::Plugin, public flutter::StreamHandler<EncodableValue>
    {
    public:
        static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

        ReactiveBleWindowsPlugin(flutter::PluginRegistrarWindows *registrar);

        virtual ~ReactiveBleWindowsPlugin();

    private:
        void HandleMethodCall(
            const flutter::MethodCall<flutter::EncodableValue> &method_call,
            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

        std::unique_ptr<flutter::StreamHandlerError<>> OnListenInternal(
            const EncodableValue *arguments,
            std::unique_ptr<flutter::EventSink<>> &&events) override;

        std::unique_ptr<flutter::StreamHandlerError<>> OnCancelInternal(
            const EncodableValue *arguments) override;

        winrt::fire_and_forget ConnectAsync(uint64_t addr);

        concurrency::task<std::list<DiscoveredService>> DiscoverServicesAsync(BluetoothDeviceAgent &bluetoothDeviceAgent);

        void BluetoothLEDevice_ConnectionStatusChanged(BluetoothLEDevice sender, IInspectable args);

        concurrency::task<void> CleanConnection(uint64_t bluetoothAddress);

        void SendConnectionUpdate(std::string address, DeviceConnectionState state);

        template <typename T>
        std::shared_ptr<EncodableList> MessageToEncodableList(T data);

        template <typename T>
        std::pair<std::unique_ptr<T>, std::string> ParseArgsToRequest(const flutter::EncodableValue *args);

        concurrency::task<std::shared_ptr<GattReadResult>> ReadCharacteristicAsync(CharacteristicAddress &charAddr);

        concurrency::task<std::shared_ptr<GattCommunicationStatus>> WriteCharacteristicAsync(CharacteristicAddress &charAddr,
            std::vector<uint8_t> value, bool withResponse);

        concurrency::task<NegotiateMtuInfo> NegotiateMtuAsync(BluetoothDeviceAgent &bluetoothDeviceAgent, uint32_t mtuSize);

        // Variables

        std::vector<winrt::hstring> scanParams;

        std::unique_ptr<flutter::EventSink<EncodableValue>> connected_device_sink_;
        std::map<uint64_t, std::shared_ptr<BluetoothDeviceAgent>> connectedDevices{};

        CharacteristicAddress characteristicAddress;
        winrt::Windows::Storage::Streams::IBuffer characteristicBuffer;
        flutter::EventSink<EncodableValue>* characteristicSink;
        flutter::CallingMethod callingMethod;
    };


    /**
     * @brief Registers this plugin with the registrar, and instantiates channels and associated handlers.
     * 
     * @param registrar Windows-specific plugin registrar.
     */
    void ReactiveBleWindowsPlugin::RegisterWithRegistrar(
        flutter::PluginRegistrarWindows *registrar)
    {
        auto methodChannel =
            std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
                registrar->messenger(), "flutter_reactive_ble_method",
                &flutter::StandardMethodCodec::GetInstance());

        auto connectedChannel =
            std::make_unique<flutter::EventChannel<EncodableValue>>(
                registrar->messenger(), "flutter_reactive_ble_connected_device",
                &flutter::StandardMethodCodec::GetInstance());

        auto statusChannel =
            std::make_unique<flutter::EventChannel<EncodableValue>>(
                registrar->messenger(), "flutter_reactive_ble_status",
                &flutter::StandardMethodCodec::GetInstance());

        auto plugin = std::make_unique<ReactiveBleWindowsPlugin>(registrar);

        methodChannel->SetMethodCallHandler(
            [plugin_pointer = plugin.get()](const auto &call, auto result)
            {
                plugin_pointer->HandleMethodCall(call, std::move(result));
            });

        auto handler = std::make_unique<
            flutter::StreamHandlerFunctions<>>(
            [plugin_pointer = plugin.get()](
                const EncodableValue *arguments,
                std::unique_ptr<flutter::EventSink<>> &&events)
                -> std::unique_ptr<flutter::StreamHandlerError<>> {
                return plugin_pointer->OnListen(arguments, std::move(events));
            },
            [plugin_pointer = plugin.get()](const EncodableValue *arguments)
                -> std::unique_ptr<flutter::StreamHandlerError<>> {
                return plugin_pointer->OnCancel(arguments);
            });

        auto statusHandler = std::make_unique<flutter::BleStatusHandler>();

        connectedChannel->SetStreamHandler(std::move(handler));
        statusChannel->SetStreamHandler(std::move(statusHandler));

        registrar->AddPlugin(std::move(plugin));
    }


    ReactiveBleWindowsPlugin::ReactiveBleWindowsPlugin(flutter::PluginRegistrarWindows *registrar)
    {
        auto scanChannel =
            std::make_unique<flutter::EventChannel<EncodableValue>>(
                registrar->messenger(), "flutter_reactive_ble_scan",
                &flutter::StandardMethodCodec::GetInstance());
        std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> scanHandler =
            std::make_unique<flutter::BleScanHandler>(&scanParams);
        scanChannel->SetStreamHandler(std::move(scanHandler));

        characteristicBuffer = IBuffer();
        callingMethod = flutter::CallingMethod::none;

        flutter::CharHandlerPtrs ptrs;
        ptrs.address = &characteristicAddress;
        ptrs.buffer = &characteristicBuffer;
        ptrs.callingMethod = &callingMethod;
        ptrs.connectedDevices = &connectedDevices;

        auto characteristicChannel =
            std::make_unique<flutter::EventChannel<EncodableValue>>(
                registrar->messenger(), "flutter_reactive_ble_char_update",
                &flutter::StandardMethodCodec::GetInstance());
        std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> charHandler =
            std::make_unique<flutter::BleCharHandler>(ptrs);
        characteristicChannel->SetStreamHandler(std::move(charHandler));
    }


    ReactiveBleWindowsPlugin::~ReactiveBleWindowsPlugin() {}


    /**
     * @brief Handler for method calls from Flutter on the method channel.
     * 
     * @param method_call The method call from Flutter.
     * @param result The result of the method call, which will be success, error, or not implemented.
     */
    void ReactiveBleWindowsPlugin::HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue> &method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
    {
        std::string methodName = method_call.method_name();
        if (methodName.compare("initialize") == 0)
        {
            result->Success();
        }
        else if (methodName.compare("deinitialize") == 0)
        {
            result->Success();
        }
        else if (methodName.compare("scanForDevices") == 0)
        {
            //TODO: Use scan parameter List<Uuid> withServices?
            // Note: Unless there is some way of filtering via the device watcher which I
            //       have not been able to find, for such filtering to occur a connection
            //       must be established with the advertising device and the service UUIDs
            //       subsequently obtained. This would likely significantly slow down scanning.
            //       Alternatively - revert to BluetoothLEAdvertisementWatcher with potentially
            //       unresolved connectivity issues.

            // scanMode and requireLocationServicesEnabled are Android specific, so are ignored.
            scanParams.clear();
            std::pair<std::unique_ptr<ScanForDevicesRequest>, std::string> parseResult =
                ParseArgsToRequest<ScanForDevicesRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            for (auto uuid : parseResult.first->serviceuuids())
            {
                std::string uuidStr = BleUtils::ProtobufUuidToString(uuid);
                scanParams.push_back(winrt::to_hstring(uuidStr));
            }
            result->Success();
        }
        else if (methodName.compare("connectToDevice") == 0)
        {
            std::pair<std::unique_ptr<ConnectToDeviceRequest>, std::string> parseResult =
                ParseArgsToRequest<ConnectToDeviceRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            auto addr = std::stoull(parseResult.first->deviceid());
            ConnectAsync(addr);
            result->Success();
        }
        else if (methodName.compare("disconnectFromDevice") == 0)
        {
            std::pair<std::unique_ptr<DisconnectFromDeviceRequest>, std::string> parseResult =
                ParseArgsToRequest<DisconnectFromDeviceRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            uint64_t addr = std::stoull(parseResult.first->deviceid());
            CleanConnection(addr).get();
            result->Success();
        }
        else if (methodName.compare("readCharacteristic") == 0)
        {
            std::pair<std::unique_ptr<ReadCharacteristicRequest>, std::string> parseResult =
                ParseArgsToRequest<ReadCharacteristicRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            characteristicAddress = parseResult.first->characteristic();
            auto task { ReadCharacteristicAsync(characteristicAddress) };
            std::shared_ptr<GattReadResult> readResult = task.get();
            if (!readResult)
            {
                // Null
                result->Error("Not currently connected to selected device");
                return;
            }

            switch (readResult->Status())
            {
            case GattCommunicationStatus::Unreachable:
                result->Error("Error reading characteristic: Device unreachable.");
                return;

            case GattCommunicationStatus::ProtocolError:
                result->Error("Error reading characteristic: Protocol error.");
                return;

            case GattCommunicationStatus::AccessDenied:
                result->Error("Error reading characteristic: Access denied.");
                return;

            case GattCommunicationStatus::Success:
                // Continue with reading the characteristic
                break;
            
            default:
                result->Error("Unknown error occurred reading characteristic.");
                return;
            }
            characteristicBuffer = readResult->Value();
            callingMethod = flutter::CallingMethod::read;
            result->Success();
        }
        else if (methodName.compare("writeCharacteristicWithResponse") == 0
                || methodName.compare("writeCharacteristicWithoutResponse") == 0)
        {
            bool withResponse = methodName.compare("writeCharacteristicWithResponse") == 0;
            std::pair<std::unique_ptr<WriteCharacteristicRequest>, std::string> parseResult =
                ParseArgsToRequest<WriteCharacteristicRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            CharacteristicAddress charAddr = parseResult.first->characteristic();
            std::string value = parseResult.first->value();
            std::vector<uint8_t> bytes(value.begin(), value.end());

            auto task { WriteCharacteristicAsync(charAddr, bytes, withResponse) };
            std::shared_ptr<GattCommunicationStatus> writeStatus = task.get();

            if (writeStatus == nullptr || *writeStatus != GattCommunicationStatus::Success)
            {
                result->Error("Failed to write characteristic.");
                return;
            }

            WriteCharacteristicInfo info;
            info.mutable_characteristic()->CopyFrom(charAddr);
            auto encoded = ReactiveBleWindowsPlugin::MessageToEncodableList(info);
            if (encoded == nullptr)
            {
                result->Error("Failed to serialize message into buffer.");
                return;
            }
            result->Success(*encoded);
        }
        else if (methodName.compare("readNotifications") == 0 || methodName.compare("stopNotifications") == 0)
        {
            std::pair<std::unique_ptr<NotifyCharacteristicRequest>, std::string> parseResult =
                ParseArgsToRequest<NotifyCharacteristicRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            characteristicAddress = parseResult.first->characteristic();
            callingMethod = (methodName.compare("readNotifications") == 0) ?
                flutter::CallingMethod::subscribe : flutter::CallingMethod::unsubscribe;
            result->Success();  // Hand-over to characteristic handler
        }
        else if (methodName.compare("negotiateMtuSize") == 0)
        {
            std::pair<std::unique_ptr<NegotiateMtuRequest>, std::string> parseResult =
                ParseArgsToRequest<NegotiateMtuRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }
            std::string deviceID = parseResult.first->deviceid();
            uint64_t addr = std::stoull(deviceID);
            auto iter = connectedDevices.find(addr);
            if (iter == connectedDevices.end())
            {
                result->Error("Not currently connected to selected device");
                return;
            }
            auto mtuSize = parseResult.first->mtusize();
            auto task { NegotiateMtuAsync(*iter->second, (uint32_t)mtuSize) };
            NegotiateMtuInfo mtuInfo = task.get();
            auto encoded = ReactiveBleWindowsPlugin::MessageToEncodableList(mtuInfo);
            if (encoded == nullptr)
            {
                result->Error("Failed to serialize message into buffer.");
                return;
            }
            result->Success(*encoded);
        }
        else if (methodName.compare("requestConnectionPriority") == 0)  // data
        {
            result->NotImplemented();
        }
        else if (methodName.compare("clearGattCache") == 0)  // data
        {
            result->NotImplemented();
        }
        else if (methodName.compare("discoverServices") == 0)
        {
            std::pair<std::unique_ptr<DiscoverServicesRequest>, std::string> parseResult =
                ParseArgsToRequest<DiscoverServicesRequest>(method_call.arguments());
            if (!parseResult.second.empty())
            {
                result->Error(parseResult.second);
                return;
            }

            std::string deviceID = parseResult.first->deviceid();
            uint64_t addr = std::stoull(deviceID);
            auto iter = connectedDevices.find(addr);
            if (iter == connectedDevices.end())
            {
                result->Error("Not currently connected to selected device");
                return;
            }

            auto task { DiscoverServicesAsync(*iter->second) };
            std::list<DiscoveredService> data = task.get();

            DiscoverServicesInfo info;
            info.set_deviceid(deviceID);
            for (DiscoveredService service : data)
            {
                info.add_services()->CopyFrom(service);
            }

            size_t size = info.ByteSizeLong();
            uint8_t *buffer = (uint8_t *)malloc(size);
            bool success = info.SerializeToArray(buffer, size);
            if (!success)
            {
                free(buffer);
                result->Error("Failed to serialize message into buffer.");
            }

            EncodableList encoded;
            for (uint32_t i = 0; i < size; i++)
            {
                uint8_t value = buffer[i];
                EncodableValue encodedVal = (EncodableValue)value;
                encoded.push_back(encodedVal);
            }
            free(buffer);
            result->Success(encoded);
            return;
        }
        else
        {
            std::cout << "Unknown method: " << methodName << std::endl; // Debugging
            result->NotImplemented();
        }
    }


    /**
     * @brief Handler for OnListen to device connection.
     * 
     * @param arguments Currently unused parameter required by interface.
     * @param events Unique pointer to the event sink for connected devices.
     * @return std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> 
     */
    std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> ReactiveBleWindowsPlugin::OnListenInternal(
        const EncodableValue *arguments, std::unique_ptr<flutter::EventSink<EncodableValue>> &&events)
    {
        connected_device_sink_ = std::move(events);
        return nullptr;
    }


    /**
     * @brief Handler for cancelling device connection.
     * 
     * @param arguments Currently unused parameter required by interface.
     * @return std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> 
     */
    std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> ReactiveBleWindowsPlugin::OnCancelInternal(
        const EncodableValue *arguments)
    {
        connected_device_sink_ = nullptr;
        return nullptr;
    }


    /**
     * @brief Asynchronously connect to the BLE device with given address.
     * 
     * @param addr The address of the BLE device to connect to.
     * @return winrt::fire_and_forget
     */
    winrt::fire_and_forget ReactiveBleWindowsPlugin::ConnectAsync(uint64_t deviceAddr)
    {
        try
        {
            BluetoothLEDevice device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(deviceAddr);

            if (device == nullptr)
            {
                OutputDebugString((L"FromBluetoothAddressAsync error: Could not find device identified by "
                    + winrt::to_hstring(deviceAddr) + L"\n").c_str());
                SendConnectionUpdate(std::to_string(deviceAddr), DeviceConnectionState::disconnected);
                co_return;
            }
            device.RequestPreferredConnectionParameters(BluetoothLEPreferredConnectionParameters::ThroughputOptimized());

            if (device != nullptr)
            {
                auto servicesResult = co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);

                if (servicesResult.Status() != GattCommunicationStatus::Success)
                {
                    OutputDebugString((L"GetGattServicesAsync error: " + winrt::to_hstring((int32_t)servicesResult.Status())
                        + L"\n").c_str());
                    SendConnectionUpdate(std::to_string(deviceAddr), DeviceConnectionState::disconnected);
                    co_return;
                }

                auto connnectionStatusChangedToken = device.ConnectionStatusChanged({
                    this, &ReactiveBleWindowsPlugin::BluetoothLEDevice_ConnectionStatusChanged
                });
                auto deviceAgent = std::make_unique<BluetoothDeviceAgent>(device, connnectionStatusChangedToken);
                auto pair = std::make_pair(deviceAddr, std::move(deviceAgent));
                connectedDevices.insert(std::move(pair));
                SendConnectionUpdate(std::to_string(deviceAddr), DeviceConnectionState::connected);
                co_return;
            }
        }
        catch (winrt::hresult_error &ex)
        {
            if (ex.to_abi() == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE))
            {
                OutputDebugString(L"Bluetooth radio is not on");
            }
            else
            {
                throw;
            }
        }
    }


    /**
     * @brief Create a task to asyncrhonously obtain the services of the connected BLE device.
     * 
     * @param bluetoothDeviceAgent Agent for the BLE device.
     * @return concurrency::task<std::list<DiscoveredService>> Asynchronous object which will
     *         return a list of discovered services from the BLE device.
     */
    concurrency::task<std::list<DiscoveredService>> ReactiveBleWindowsPlugin::DiscoverServicesAsync(BluetoothDeviceAgent &bluetoothDeviceAgent)
    {
        return concurrency::create_task([bluetoothDeviceAgent]
        {
            auto servicesResult = bluetoothDeviceAgent.device.GetGattServicesAsync().get();
            std::list<DiscoveredService> result;
            if (servicesResult.Status() != GattCommunicationStatus::Success)
            {
                OutputDebugString((L"GetGattServicesAsync error: " + winrt::to_hstring((int32_t)servicesResult.Status())
                    + L"\n").c_str());
                return result;
            }
            winrt::Windows::Foundation::Collections::IVectorView<GattDeviceService> services = servicesResult.Services();

            for (size_t i = 0; i < services.Size(); i++)
            {
                DiscoveredService converted;
                GattDeviceService const service = services.GetAt(i);
                std::vector<uint8_t> serviceUuidBytes = BleUtils::GuidToByteVec(service.Uuid());
                for (auto p = serviceUuidBytes.begin(); p != serviceUuidBytes.end(); p++)
                    converted.mutable_serviceuuid()->mutable_data()->push_back(*p);

                winrt::Windows::Foundation::Collections::IVectorView includedServices =
                    service.GetIncludedServicesAsync().get().Services();
                converted.add_includedservices();
                for (size_t j = 0; j < includedServices.Size(); j++)            
                {
                    DiscoveredService tmp;
                    std::vector<uint8_t> includedUuidBytes =
                        BleUtils::GuidToByteVec(includedServices.GetAt(j).Uuid());
                    for (auto q = includedUuidBytes.begin(); q != includedUuidBytes.end(); q++)
                        tmp.mutable_serviceuuid()->mutable_data()->push_back(*q);
                    converted.add_includedservices()->CopyFrom(tmp);
                }

                winrt::Windows::Foundation::Collections::IVectorView characteristics =
                    service.GetCharacteristicsAsync().get().Characteristics();
                for (size_t j = 0; j < characteristics.Size(); j++)            
                {
                    GattCharacteristic tmp_char = characteristics.GetAt(j);
                    GattCharacteristicProperties props = tmp_char.CharacteristicProperties();
                    DiscoveredCharacteristic tmp;

                    std::vector<uint8_t> charUuidBytes = BleUtils::GuidToByteVec(tmp_char.Uuid());
                    for (auto r = charUuidBytes.begin(); r != charUuidBytes.end(); r++)
                        tmp.mutable_characteristicid()->mutable_data()->push_back(*r);

                    for (auto s = serviceUuidBytes.begin(); s != serviceUuidBytes.end(); s++)
                        tmp.mutable_serviceid()->mutable_data()->push_back(*s);

                    tmp.set_isreadable((props & GattCharacteristicProperties::Read) != GattCharacteristicProperties::None);
                    tmp.set_iswritablewithresponse((props & GattCharacteristicProperties::Write) != GattCharacteristicProperties::None);
                    tmp.set_iswritablewithoutresponse((props & GattCharacteristicProperties::WriteWithoutResponse) != GattCharacteristicProperties::None);
                    tmp.set_isnotifiable((props & GattCharacteristicProperties::Notify) != GattCharacteristicProperties::None);
                    tmp.set_isindicatable((props & GattCharacteristicProperties::Indicate) != GattCharacteristicProperties::None);

                    converted.add_characteristics()->CopyFrom(tmp);
                    converted.add_characteristicuuids()->CopyFrom(tmp.characteristicid());
                }

                result.push_back(converted);
            }
            return result;
        });
    }


    /**
     * @brief Handler for "connection status changed" event on connected BLE device.
     *        Currently only acts on change to the disconnected state.
     * 
     * @param sender The connected BLE device whose status changed.
     * @param args Unused parameter required by the interface.
     */
    void ReactiveBleWindowsPlugin::BluetoothLEDevice_ConnectionStatusChanged(BluetoothLEDevice sender, IInspectable args)
    {
        OutputDebugString((L"ConnectionStatusChanged " + winrt::to_hstring((int32_t)sender.ConnectionStatus())
            + L"\n").c_str());
        if (sender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
        {
            CleanConnection(sender.BluetoothAddress()).get();
            SendConnectionUpdate(std::to_string(sender.BluetoothAddress()), DeviceConnectionState::disconnected);
        }
    }


    /**
     * @brief Cleans up/disconnects from a BLE device with given address.
     * 
     * Attempts to indicate to the device to stop sending notifications/indications, but if communication
     * fails the method will ignore the failure and continue.
     * 
     * @param bluetoothAddress The BLE device's address.
     */
    concurrency::task<void> ReactiveBleWindowsPlugin::CleanConnection(uint64_t bluetoothAddress)
    {
        return concurrency::create_task([this, bluetoothAddress]
        {
            auto node = connectedDevices.extract(bluetoothAddress);
            if (!node.empty())
            {
                std::shared_ptr<BluetoothDeviceAgent> agent = std::move(node.mapped());
                agent->device.ConnectionStatusChanged(agent->connnectionStatusChangedToken);
                for (auto &tokenPair : agent->subscribedCharacteristicsAndTokens)
                {
                    std::pair<GattCharacteristic, winrt::event_token> charAndToken = tokenPair.second;
                    // Unregister the handler and tell the device to stop sending
                    charAndToken.first.ValueChanged(charAndToken.second);
                    charAndToken.first.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::None).get();
                }
                agent->device.Close();
            }
        });
    }


    /**
     * @brief Sends an update on the connection status of a BLE device to the connected device channel.
     * 
     * @param address Address of the BLE device which the update is for
     * @param state State of the BLE device, following the DeviceConnectionState enum from the
     *              `reactive_ble_platform_interface` package's `connection_state_update.dart`.
     */
    void ReactiveBleWindowsPlugin::SendConnectionUpdate(std::string address, DeviceConnectionState state)
    {
        DeviceInfo info;
        info.set_id(address);
        info.set_connectionstate(state);
        auto result = ReactiveBleWindowsPlugin::MessageToEncodableList(info);
        if (result == nullptr) return;
        connected_device_sink_->EventSink::Success(*result);
    }


    /**
     * @brief Serializes a google::protobuf::Message into a Flutter EncodableList.
     * 
     * @tparam T The type of Message being serialized.
     * @param data The Message of type T to serialize.
     * @return std::shared_ptr<EncodableList> Pointer to the EncodableList containing the serialized Message, or nullptr on error.
     */
    template <typename T>
    std::shared_ptr<EncodableList> ReactiveBleWindowsPlugin::MessageToEncodableList(T data)
    {
        size_t size = data.ByteSizeLong();
        uint8_t *buffer = (uint8_t *)malloc(size);
        bool success = data.SerializeToArray(buffer, size);
        if (!success)
        {
            std::cout << "Failed to serialize message into buffer." << std::endl; // Debugging
            free(buffer);
            return nullptr;
        }

        EncodableList result;
        for (uint32_t i = 0; i < size; i++)
        {
            uint8_t value = buffer[i];
            EncodableValue encodedVal = (EncodableValue)value;
            result.push_back(encodedVal);
        }
        free(buffer);

        return std::make_shared<EncodableList>(result);
    }


    /**
     * @brief Parse Flutter method call arguments into request of type T.
     * 
     * @tparam T The type of request to parse the arguments into, must implement google::protobuf::Message.
     * @param args Method call arguments to parse into request of type T.
     * @return std::pair<T*, std::string> Pair of pointer to the parsed args, and an error message string
     *                                    (in case of error pointer will be null and string non-empty).
     */
    template <typename T>
    std::pair<std::unique_ptr<T>, std::string> ReactiveBleWindowsPlugin::ParseArgsToRequest(const flutter::EncodableValue *args)
    {
        // std::get_if returns a pointer to the value stored or a null pointer on error.
        // This ensures we return early if we get a null pointer.
        const std::vector<uint8_t> *pVector = std::get_if<std::vector<uint8_t>>(args);
        if (pVector && pVector->size() <= 0)
        {
            return std::make_pair(nullptr, "No data in message.");
        }

        // Parse vector into a protobuf message via ParsePartialFromArray.
        // Note the call to `pVector->data()` (https://en.cppreference.com/w/cpp/container/vector/data),
        // and note further that this may return a null pointer if pVector->size() is 0 (hence prior check).
        auto req = std::make_unique<T>();
        bool res = req->ParsePartialFromArray(pVector->data(), pVector->size());
        if (res)
        {
            return std::make_pair(std::move(req), "");
        }

        // ParsePartialFromArray returned false, indicating it was unable to parse the given data.
        return std::make_pair(nullptr, "Unable to parse device address.");
    }


    /**
     * @brief Asynchronously get information from the relevant connected device on the characteristic with given address.
     * 
     * Returns a shared pointer such that a nullpointer may be returned on error.
     * Cannot return a unique pointer, as it will fail to compile - attempting to reference a deleted function.
     * 
     * @param charAddr Address of the characteristic to get info on (contains device ID, service ID, and characteristic ID).
     * @return concurrency::task<std::shared_ptr<GattReadResult>> Shared pointer to the returned GATT result, may be nullptr.
     */
    concurrency::task<std::shared_ptr<GattReadResult>> ReactiveBleWindowsPlugin::ReadCharacteristicAsync(CharacteristicAddress &charAddr)
    {
        return concurrency::create_task([this, charAddr]
        {
            uint64_t deviceAddr = std::stoull(charAddr.deviceid());
            auto iter = connectedDevices.find(deviceAddr);
            if (iter == connectedDevices.end())
            {
                return std::shared_ptr<GattReadResult>(nullptr);
            }

            std::string serviceUuid = BleUtils::ProtobufUuidToString(charAddr.serviceuuid());
            std::string charUuid = BleUtils::ProtobufUuidToString(charAddr.characteristicuuid());
            auto gattChar = (*iter->second).GetCharacteristicAsync(serviceUuid, charUuid).get();
            auto readResult = gattChar.ReadValueAsync().get();
            return std::make_shared<GattReadResult>(readResult);
        });
    }


    /**
     * @brief Asynchronously write the new value to the characteristic with given address.
     * 
     * Returns a shared pointer such that a nullpointer may be returned on error.
     * Cannot return a unique pointer, as it will fail to compile - attempting to reference a deleted function.
     * 
     * @param charAddr Address of the characteristic to get info on (contains device ID, service ID, and characteristic ID).
     * @param value The new value for the characteristic.
     * @param withResponse If the write operation should return a response.
     * @return concurrency::task<std::shared_ptr<GattCommunicationStatus>> Shared pointer to the returned GATT communication status, may be nullptr.
     */
    concurrency::task<std::shared_ptr<GattCommunicationStatus>> ReactiveBleWindowsPlugin::WriteCharacteristicAsync(
        CharacteristicAddress &charAddr, std::vector<uint8_t> value, bool withResponse)
    {
        return concurrency::create_task([this, charAddr, value, withResponse]
        {
            uint64_t addr = std::stoull(charAddr.deviceid());
            auto iter = connectedDevices.find(addr);
            if (iter == connectedDevices.end())
            {
                return std::shared_ptr<GattCommunicationStatus>(nullptr);
            }

            std::string serviceUuid = BleUtils::ProtobufUuidToString(charAddr.serviceuuid());
            std::string charUuid = BleUtils::ProtobufUuidToString(charAddr.characteristicuuid());
            auto gattChar = (*iter->second).GetCharacteristicAsync(serviceUuid, charUuid).get();
            DataWriter writer;
            writer.WriteBytes(value);
            IBuffer buf = writer.DetachBuffer();

            try {
                GattCommunicationStatus writeStatus =
                    gattChar.WriteValueAsync(buf, (withResponse) ?
                        GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse).get();
                return std::make_shared<GattCommunicationStatus>(writeStatus);
            } catch (...) {
                std::cerr << "Failed to write to characteristic. Can it be written to?" << std::endl;  // Debugging
                return std::shared_ptr<GattCommunicationStatus>(nullptr);
            }
        });
    }


    /**
     * @brief MTU size on Windows cannot be negotiated, so this method returns the device's current MTU size.
     * 
     * @param bluetoothDeviceAgent Agent for the BLE device.
     * @param mtuSize The requested MTU size, unused as MTU size cannot be negotiated on Windows.
     * @return concurrency::task<NegotiateMtuInfo> A NegotiateMtuInfo containing the current MTU size.
     */
    concurrency::task<NegotiateMtuInfo> ReactiveBleWindowsPlugin::NegotiateMtuAsync(BluetoothDeviceAgent &bluetoothDeviceAgent, uint32_t mtuSize)
    {
        return concurrency::create_task([bluetoothDeviceAgent, mtuSize]
        {
            NegotiateMtuInfo mtuInfo;
            auto deviceID = bluetoothDeviceAgent.device.BluetoothDeviceId();
            mtuInfo.mutable_deviceid()->assign(to_string(deviceID.Id()));
            auto gattSession = GattSession::FromDeviceIdAsync(deviceID).get();
            mtuInfo.set_mtusize(gattSession.MaxPduSize());  // will set_mtusize work?
            return mtuInfo;
        });
    }

} // namespace


void ReactiveBleWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar)
{
    ReactiveBleWindowsPlugin::RegisterWithRegistrar(
        flutter::PluginRegistrarManager::GetInstance()
            ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
