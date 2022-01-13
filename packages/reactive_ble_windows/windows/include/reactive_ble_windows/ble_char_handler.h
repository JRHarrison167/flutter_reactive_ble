#ifndef BLE_CHAR_HANDLER_H
#define BLE_CHAR_HANDLER_H

#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/event_stream_handler.h>
#include <flutter/standard_method_codec.h>

#include <windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#include "../lib/src/generated/bledata.pb.h"

namespace flutter
{
    /*
    *  As the characteristic handler's OnListen/OnCancel methods get called for multiple method calls,
    *  this enum is used to differentiate between which method was called and hence how the handler should behave.
    * 
    *  TODO: Can this just be a bool? Is special handling needed in any case other than read vs not read?
    */
    enum CallingMethod
    {
        read,
        notify,
        unsubscribe,
        none
    };


    /*
    *  Container for the pointers which get passed to the characteristic handler.
    */
    struct CharHandlerPtrs
    {
        CharacteristicAddress* address;
        winrt::Windows::Storage::Streams::IBuffer* buffer;
        std::shared_ptr<flutter::EventSink<EncodableValue>> sink;
        CallingMethod* callingMethod;
    };


    class BleCharHandler : public StreamHandler<EncodableValue>
    {
    public:
        BleCharHandler() {}
        virtual ~BleCharHandler() = default;

        BleCharHandler(CharHandlerPtrs ptrs)
        {
            characteristicAddress = ptrs.address;
            characteristicBuffer = ptrs.buffer;
            characteristic_sink_ = ptrs.sink;
            callingMethod = ptrs.callingMethod;
        };

        // Prevent copying.
        BleCharHandler(BleCharHandler const &) = delete;
        BleCharHandler &operator=(BleCharHandler const &) = delete;

    protected:
        virtual std::unique_ptr<StreamHandlerError<>> OnListenInternal(
            const EncodableValue *arguments,
            std::unique_ptr<EventSink<EncodableValue>> &&events);

        virtual std::unique_ptr<StreamHandlerError<>> OnCancelInternal(
            const EncodableValue *arguments);

        void SendCharacteristicInfo();

        CharacteristicAddress* characteristicAddress;
        winrt::Windows::Storage::Streams::IBuffer* characteristicBuffer;
        std::shared_ptr<flutter::EventSink<EncodableValue>> characteristic_sink_;
        CallingMethod* callingMethod;
    };

} // namespace flutter

#endif // BLE_CHAR_HANDLER_H
