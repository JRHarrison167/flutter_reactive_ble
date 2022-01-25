#include "include/reactive_ble_windows/ble_scan_handler.h"
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
namespace flutter
{
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;
    using namespace winrt::Windows::Devices::Radios;
    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
    using namespace winrt::Windows::Storage::Streams;


    union uint16_t_union
    {
        uint16_t uint16;
        byte bytes[sizeof(uint16_t)];
    };


    /**
     * @brief Converts IBuffer to byte vector.
     * 
     * @param buffer The buffer to be converted.
     * @return std::vector<uint8_t> The buffer contents as bytes.
     */
    std::vector<uint8_t> to_bytevc(IBuffer buffer)
    {
        auto reader = DataReader::FromBuffer(buffer);
        auto result = std::vector<uint8_t>(reader.UnconsumedBufferLength());
        reader.ReadBytes(result);
        return result;
    }


    /**
     * @brief Parse advertised manufacturer data into bytes.
     * 
     * @param advertisement The BLE advertisement containing manufacturer data.
     * @return std::vector<uint8_t> The parsed manufacturer data as bytes.
     */
    std::vector<uint8_t> parseManufacturerData(BluetoothLEAdvertisement advertisement)
    {
        if (advertisement.ManufacturerData().Size() == 0) return std::vector<uint8_t>();

        auto manufacturerData = advertisement.ManufacturerData().GetAt(0);
        // FIXME Compat with REG_DWORD_BIG_ENDIAN
        uint8_t *prefix = uint16_t_union{manufacturerData.CompanyId()}.bytes;
        auto result = std::vector<uint8_t>{prefix, prefix + sizeof(uint16_t_union)};

        auto data = to_bytevc(manufacturerData.Data());
        result.insert(result.end(), data.begin(), data.end());
        return result;
    }


    /**
     * @brief Handler for OnListen to BLE scanning for devices.
     * 
     * @param arguments Currently unused parameter required by interface.
     * @param events Unique pointer to the event sink for BLE advertisements.
     * @return std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> 
     */
    std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> BleScanHandler::OnListenInternal(
        const EncodableValue *arguments, std::unique_ptr<flutter::EventSink<EncodableValue>> &&events)
    {
        scan_result_sink_ = std::move(events);
        bleWatcher = BluetoothLEAdvertisementWatcher();
        bluetoothLEWatcherReceivedToken = bleWatcher.Received({this, &BleScanHandler::OnAdvertisementReceived});
        bleWatcher.Start();
        initialized = true;
        return nullptr;
    }


    /**
     * @brief Handler for cancelling scanning for BLE devices.
     * 
     * @param arguments Currently unused parameter required by interface.
     * @return std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> 
     */
    std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> BleScanHandler::OnCancelInternal(
        const EncodableValue *arguments)
    {
        scan_result_sink_ = nullptr;
        if (initialized && bleWatcher.Status() == BluetoothLEAdvertisementWatcherStatus::Started)
        {
            bleWatcher.Stop();
            bleWatcher.Received(bluetoothLEWatcherReceivedToken);
            bleWatcher = nullptr;
        }
        return nullptr;
    }


    /**
     * @brief Callback for when a BLE advertisement has been received.
     * 
     * @param watcher The BLE watcher which received the advertisement.
     * @param args The arguments of the received BLE advertisement.
     */
    void BleScanHandler::OnAdvertisementReceived(
        BluetoothLEAdvertisementWatcher watcher,
        BluetoothLEAdvertisementReceivedEventArgs args)
    {
        if (scan_result_sink_)
        {
            if(static_cast<int>(args.AdvertisementType()) > 1) {
                return;
            }
            std::string btAddress = std::to_string(args.BluetoothAddress());
            if(this->knownDevices.count(btAddress) > 0) {
                return;
            }
            this->knownDevices.insert(btAddress);

            auto manufacturer_data = parseManufacturerData(args.Advertisement());

            std::stringstream sstream;
            sstream << std::hex << args.BluetoothAddress();
            std::string localName = winrt::to_string(args.Advertisement().LocalName());

            DeviceScanInfo info;

            info.set_id(std::to_string(args.BluetoothAddress()));
            // If the local name is empty, use a hex representation of the device address

            if(localName.empty()) {
                std::cout << "Getting name... " << std::endl;
                BluetoothLEDevice device = BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress()).get();
                auto services = device.GetGattServicesAsync().get();
                for (GenericAttributeProfile::GattDeviceService _tmp : services.Services()) {
                    if(_tmp.Uuid() == GenericAttributeProfile::GattServiceUuids::GenericAccess()) {
                        std::cout << "Got DInformation" << std::endl;
                        auto chars = _tmp.GetCharacteristicsAsync().get();
                        for (GenericAttributeProfile::GattCharacteristic _tmp2 : chars.Characteristics()) {
                            //if(_tmp2.Uuid() == GenericAttributeProfile::GattCharacteristicUuids::GapDeviceName()) {
                            if(_tmp2.Uuid().Data1 == 10752) {
                                std::cout << "Got Dname" << std::endl;
                                auto reader = DataReader::FromBuffer(_tmp2.ReadValueAsync().get().Value());
                                localName = winrt::to_string(reader.ReadString(reader.UnconsumedBufferLength()));

                                auto value = _tmp2.ReadValueAsync().get();
                        IBuffer val = value.Value();

                        auto reader2 = DataReader::FromBuffer(val);
                        auto result = std::vector<uint8_t>(reader2.UnconsumedBufferLength());
                        reader2.ReadBytes(result);
                        std::string txt (result.begin(), result.end());
                        std::cout << txt << std::endl;
                            }
                        }
                        break;
                    }
                }
    	        std::cout << "Got name! " << std::endl;
            }
            info.set_name((localName.empty()) ? sstream.str() : localName);
            info.set_manufacturerdata(std::string(manufacturer_data.begin(), manufacturer_data.end()));
            info.add_serviceuuids();
            info.add_servicedata();
            info.set_rssi(args.RawSignalStrengthInDBm());
            SendDeviceScanInfo(info);
        }
    }


    /**
     * @brief Sends the info obtained from a BLE advertisement to the scan results channel.
     * 
     * @param msg The info of the scanned device.
     */
    void BleScanHandler::SendDeviceScanInfo(DeviceScanInfo msg)
    {
        size_t size = msg.ByteSizeLong();
        uint8_t *buffer = (uint8_t *)malloc(size);
        bool success = msg.SerializeToArray(buffer, size);
        if (!success)
        {
            std::cout << "Failed to serialize message into buffer." << std::endl; // Debugging
            free(buffer);
            return;
        }

        EncodableList result;
        for (uint32_t i = 0; i < size; i++)
        {
            uint8_t value = buffer[i];
            EncodableValue encodedVal = (EncodableValue)value;
            result.push_back(encodedVal);
        }
        scan_result_sink_->EventSink::Success(result);
        free(buffer);
    }

} // namespace flutter
