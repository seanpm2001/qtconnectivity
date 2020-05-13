/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qbluetoothdevicediscoveryagent.h"
#include "qbluetoothdevicediscoveryagent_p.h"
#include "qbluetoothaddress.h"
#include "qbluetoothuuid.h"

#ifdef CLASSIC_APP_BUILD
#define Q_OS_WINRT
#endif
#include "qfunctions_winrt.h"

#include <QtBluetooth/private/qtbluetoothglobal_p.h>
#include <QtBluetooth/private/qbluetoothutils_winrt_p.h>
#include <QtCore/QLoggingCategory>
#include <QtCore/qmutex.h>
#include <QtCore/private/qeventdispatcher_winrt_p.h>
#include <QtCore/qmutex.h>

#include <robuffer.h>
#include <wrl.h>
#include <windows.devices.enumeration.h>
#include <windows.devices.bluetooth.h>
#include <windows.foundation.collections.h>
#include <windows.storage.streams.h>

#include <windows.devices.bluetooth.advertisement.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::Devices;
using namespace ABI::Windows::Devices::Bluetooth;
using namespace ABI::Windows::Devices::Bluetooth::Advertisement;
using namespace ABI::Windows::Devices::Enumeration;
using namespace ABI::Windows::Storage::Streams;

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_BT_WINRT)

#define WARN_AND_RETURN_IF_FAILED(msg, ret) \
    if (FAILED(hr)) { \
        qCWarning(QT_BT_WINRT) << msg; \
        ret; \
    }

#define WARN_AND_CONTINUE_IF_FAILED(msg) \
    if (FAILED(hr)) { \
        qCWarning(QT_BT_WINRT) << msg; \
        continue; \
    }

static ManufacturerData extractManufacturerData(ComPtr<IBluetoothLEAdvertisement> ad)
{
    ManufacturerData ret;
    ComPtr<IVector<BluetoothLEManufacturerData*>> data;
    HRESULT hr = ad->get_ManufacturerData(&data);
    WARN_AND_RETURN_IF_FAILED("Could not obtain list of manufacturer data.", return ret);
    quint32 size;
    hr = data->get_Size(&size);
    WARN_AND_RETURN_IF_FAILED("Could not obtain manufacturer data's list size.", return ret);
    for (quint32 i = 0; i < size; ++i) {
        ComPtr<IBluetoothLEManufacturerData> d;
        hr = data->GetAt(i, &d);
        WARN_AND_CONTINUE_IF_FAILED("Could not obtain manufacturer data.");
        quint16 id;
        hr = d->get_CompanyId(&id);
        WARN_AND_CONTINUE_IF_FAILED("Could not obtain manufacturer data company id.");
        ComPtr<IBuffer> buffer;
        hr = d->get_Data(&buffer);
        WARN_AND_CONTINUE_IF_FAILED("Could not obtain manufacturer data set.");
        const QByteArray bufferData = byteArrayFromBuffer(buffer);
        if (ret.contains(id))
            qCWarning(QT_BT_WINRT) << "Company ID already present in manufacturer data.";
        ret.insert(id, bufferData);
    }
    return ret;
}

class QWinRTBluetoothDeviceDiscoveryWorker : public QObject
{
    Q_OBJECT
public:
    explicit QWinRTBluetoothDeviceDiscoveryWorker(QBluetoothDeviceDiscoveryAgent::DiscoveryMethods methods);
    ~QWinRTBluetoothDeviceDiscoveryWorker();
    void start();
    void stopLEWatcher();

private:
    void startDeviceDiscovery(QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode);
    void onDeviceDiscoveryFinished(IAsyncOperation<DeviceInformationCollection *> *op,
                                   QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode);
    void gatherDeviceInformation(IDeviceInformation *deviceInfo,
                                 QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode);
    void gatherMultipleDeviceInformation(quint32 deviceCount, IVectorView<DeviceInformation *> *devices,
                                         QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode);
    void setupLEDeviceWatcher();
    void classicBluetoothInfoFromDeviceIdAsync(HSTRING deviceId);
    void leBluetoothInfoFromDeviceIdAsync(HSTRING deviceId);
    void leBluetoothInfoFromAddressAsync(quint64 address);
    HRESULT onPairedClassicBluetoothDeviceFoundAsync(IAsyncOperation<BluetoothDevice *> *op, AsyncStatus status );
    HRESULT onPairedBluetoothLEDeviceFoundAsync(IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status);
    HRESULT onBluetoothLEDeviceFoundAsync(IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status);
    enum PairingCheck {
        CheckForPairing,
        OmitPairingCheck
    };
    HRESULT onBluetoothLEDeviceFound(ComPtr<IBluetoothLEDevice> device, PairingCheck pairingCheck);
#if QT_CONFIG(winrt_btle_no_pairing)
    HRESULT onBluetoothLEDeviceFound(ComPtr<IBluetoothLEDevice> device);
#endif

public slots:
    void finishDiscovery();

Q_SIGNALS:
    void deviceFound(const QBluetoothDeviceInfo &info);
    void deviceDataChanged(const QBluetoothAddress &address, QBluetoothDeviceInfo::Fields,
                           qint16 rssi, ManufacturerData manufacturerData);
    void scanFinished();

public:
    quint8 requestedModes;

private:
    ComPtr<IBluetoothLEAdvertisementWatcher> m_leWatcher;
    EventRegistrationToken m_leDeviceAddedToken;
#if QT_CONFIG(winrt_btle_no_pairing)
    QMutex m_foundDevicesMutex;
    struct LEAdvertisingInfo {
        QVector<QBluetoothUuid> services;
        qint16 rssi = 0;
    };

    QMap<quint64, LEAdvertisingInfo> m_foundLEDevicesMap;
#endif
    QMap<quint64, qint16> m_foundLEDevices;
    QMap<quint64, ManufacturerData> m_foundLEManufacturerData;
    int m_pendingPairedDevices;

    ComPtr<IBluetoothDeviceStatics> m_deviceStatics;
    ComPtr<IBluetoothLEDeviceStatics> m_leDeviceStatics;
};

QWinRTBluetoothDeviceDiscoveryWorker::QWinRTBluetoothDeviceDiscoveryWorker(QBluetoothDeviceDiscoveryAgent::DiscoveryMethods methods)
    : requestedModes(methods)
    , m_pendingPairedDevices(0)
{
    qRegisterMetaType<QBluetoothDeviceInfo>();
    qRegisterMetaType<QBluetoothDeviceInfo::Fields>();
    qRegisterMetaType<ManufacturerData>();

#ifdef CLASSIC_APP_BUILD
    CoInitialize(NULL);
#endif
    HRESULT hr = GetActivationFactory(HString::MakeReference(RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice).Get(), &m_deviceStatics);
    Q_ASSERT_SUCCEEDED(hr);
    hr = GetActivationFactory(HString::MakeReference(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice).Get(), &m_leDeviceStatics);
    Q_ASSERT_SUCCEEDED(hr);
}

QWinRTBluetoothDeviceDiscoveryWorker::~QWinRTBluetoothDeviceDiscoveryWorker()
{
    stopLEWatcher();
#ifdef CLASSIC_APP_BUILD
    CoUninitialize();
#endif
}

void QWinRTBluetoothDeviceDiscoveryWorker::start()
{
    QEventDispatcherWinRT::runOnXamlThread([this]() {
        if (requestedModes & QBluetoothDeviceDiscoveryAgent::ClassicMethod)
            startDeviceDiscovery(QBluetoothDeviceDiscoveryAgent::ClassicMethod);

        if (requestedModes & QBluetoothDeviceDiscoveryAgent::LowEnergyMethod) {
            startDeviceDiscovery(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
            setupLEDeviceWatcher();
        }
        return S_OK;
    });

    qCDebug(QT_BT_WINRT) << "Worker started";
}

void QWinRTBluetoothDeviceDiscoveryWorker::stopLEWatcher()
{
    if (m_leWatcher) {
        HRESULT hr = m_leWatcher->Stop();
        Q_ASSERT_SUCCEEDED(hr);
        if (m_leDeviceAddedToken.value) {
            hr = m_leWatcher->remove_Received(m_leDeviceAddedToken);
            Q_ASSERT_SUCCEEDED(hr);
        }
    }
}

void QWinRTBluetoothDeviceDiscoveryWorker::startDeviceDiscovery(QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode)
{
    HString deviceSelector;
    ComPtr<IDeviceInformationStatics> deviceInformationStatics;
    HRESULT hr = GetActivationFactory(HString::MakeReference(RuntimeClass_Windows_Devices_Enumeration_DeviceInformation).Get(), &deviceInformationStatics);
    WARN_AND_RETURN_IF_FAILED("Could not obtain device information statics", return);
    if (mode == QBluetoothDeviceDiscoveryAgent::LowEnergyMethod)
        m_leDeviceStatics->GetDeviceSelector(deviceSelector.GetAddressOf());
    else
        m_deviceStatics->GetDeviceSelector(deviceSelector.GetAddressOf());
    ComPtr<IAsyncOperation<DeviceInformationCollection *>> op;
    hr = deviceInformationStatics->FindAllAsyncAqsFilter(deviceSelector.Get(), &op);
    WARN_AND_RETURN_IF_FAILED("Could not start bluetooth device discovery operation", return);
    QPointer<QWinRTBluetoothDeviceDiscoveryWorker> thisPointer(this);
    hr = op->put_Completed(
        Callback<IAsyncOperationCompletedHandler<DeviceInformationCollection *>>([thisPointer, mode](IAsyncOperation<DeviceInformationCollection *> *op, AsyncStatus status) {
        if (status == Completed && thisPointer)
            thisPointer->onDeviceDiscoveryFinished(op, mode);
        return S_OK;
    }).Get());
    WARN_AND_RETURN_IF_FAILED("Could not add callback to bluetooth device discovery operation", return);
}

void QWinRTBluetoothDeviceDiscoveryWorker::onDeviceDiscoveryFinished(IAsyncOperation<DeviceInformationCollection *> *op, QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode)
{
    qCDebug(QT_BT_WINRT) << (mode == QBluetoothDeviceDiscoveryAgent::ClassicMethod ? "BT" : "BTLE")
        << " scan completed";
    ComPtr<IVectorView<DeviceInformation *>> devices;
    HRESULT hr;
    hr = op->GetResults(&devices);
    Q_ASSERT_SUCCEEDED(hr);
    quint32 deviceCount;
    hr = devices->get_Size(&deviceCount);
    Q_ASSERT_SUCCEEDED(hr);

    // For classic discovery only paired devices will be found. If we only do classic disovery and
    // no device is found, the scan is finished.
    if (requestedModes == QBluetoothDeviceDiscoveryAgent::ClassicMethod &&
        deviceCount == 0) {
        finishDiscovery();
        return;
    }

    m_pendingPairedDevices += deviceCount;
    gatherMultipleDeviceInformation(deviceCount, devices.Get(), mode);
}

void QWinRTBluetoothDeviceDiscoveryWorker::gatherDeviceInformation(IDeviceInformation *deviceInfo, QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode)
{
    HString deviceId;
    HRESULT hr;
    hr = deviceInfo->get_Id(deviceId.GetAddressOf());
    Q_ASSERT_SUCCEEDED(hr);
    if (mode == QBluetoothDeviceDiscoveryAgent::LowEnergyMethod)
        leBluetoothInfoFromDeviceIdAsync(deviceId.Get());
    else
        classicBluetoothInfoFromDeviceIdAsync(deviceId.Get());
}

void QWinRTBluetoothDeviceDiscoveryWorker::gatherMultipleDeviceInformation(quint32 deviceCount, IVectorView<DeviceInformation *> *devices, QBluetoothDeviceDiscoveryAgent::DiscoveryMethod mode)
{
    for (quint32 i = 0; i < deviceCount; ++i) {
        ComPtr<IDeviceInformation> device;
        HRESULT hr;
        hr = devices->GetAt(i, &device);
        Q_ASSERT_SUCCEEDED(hr);
        gatherDeviceInformation(device.Get(), mode);
    }
}

void QWinRTBluetoothDeviceDiscoveryWorker::setupLEDeviceWatcher()
{
    HRESULT hr = RoActivateInstance(HString::MakeReference(RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher).Get(), &m_leWatcher);
    Q_ASSERT_SUCCEEDED(hr);
#if QT_CONFIG(winrt_btle_no_pairing)
    if (supportsNewLEApi()) {
        hr = m_leWatcher->put_ScanningMode(BluetoothLEScanningMode_Active);
        Q_ASSERT_SUCCEEDED(hr);
    }
#endif // winrt_btle_no_pairing
    hr = m_leWatcher->add_Received(Callback<ITypedEventHandler<BluetoothLEAdvertisementWatcher *, BluetoothLEAdvertisementReceivedEventArgs *>>([this](IBluetoothLEAdvertisementWatcher *, IBluetoothLEAdvertisementReceivedEventArgs *args) {
        quint64 address;
        HRESULT hr;
        hr = args->get_BluetoothAddress(&address);
        Q_ASSERT_SUCCEEDED(hr);
        qint16 rssi;
        hr = args->get_RawSignalStrengthInDBm(&rssi);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<IBluetoothLEAdvertisement> ad;
        hr = args->get_Advertisement(&ad);
        Q_ASSERT_SUCCEEDED(hr);
        const ManufacturerData manufacturerData = extractManufacturerData(ad);
        QBluetoothDeviceInfo::Fields changedFields = QBluetoothDeviceInfo::Field::None;
        if (!m_foundLEManufacturerData.contains(address)) {
            m_foundLEManufacturerData.insert(address, manufacturerData);
            changedFields.setFlag(QBluetoothDeviceInfo::Field::ManufacturerData);
        } else if (m_foundLEManufacturerData.value(address) != manufacturerData) {
            m_foundLEManufacturerData[address] = manufacturerData;
            changedFields.setFlag(QBluetoothDeviceInfo::Field::ManufacturerData);
        }
#if QT_CONFIG(winrt_btle_no_pairing)
        if (supportsNewLEApi()) {
            ComPtr<IVector<GUID>> guids;
            hr = ad->get_ServiceUuids(&guids);
            Q_ASSERT_SUCCEEDED(hr);
            quint32 size;
            hr = guids->get_Size(&size);
            Q_ASSERT_SUCCEEDED(hr);
            QVector<QBluetoothUuid> serviceUuids;
            for (quint32 i = 0; i < size; ++i) {
                GUID guid;
                hr = guids->GetAt(i, &guid);
                Q_ASSERT_SUCCEEDED(hr);
                QBluetoothUuid uuid(guid);
                serviceUuids.append(uuid);
            }
            QMutexLocker locker(&m_foundDevicesMutex);
            // Merge newly found services with list of currently found ones
            if (m_foundLEDevicesMap.contains(address)) {
                if (size == 0)
                    return S_OK;
                const LEAdvertisingInfo adInfo = m_foundLEDevicesMap.value(address);
                QVector<QBluetoothUuid> foundServices = adInfo.services;
                if (adInfo.rssi != rssi) {
                    m_foundLEDevicesMap[address].rssi = rssi;
                    changedFields.setFlag(QBluetoothDeviceInfo::Field::RSSI);
                }
                bool newServiceAdded = false;
                for (const QBluetoothUuid &uuid : qAsConst(serviceUuids)) {
                    if (!foundServices.contains(uuid)) {
                        foundServices.append(uuid);
                        newServiceAdded = true;
                    }
                }
                if (!newServiceAdded) {
                    if (!changedFields.testFlag(QBluetoothDeviceInfo::Field::None)) {
                        QMetaObject::invokeMethod(this, "deviceDataChanged", Qt::AutoConnection,
                                                  Q_ARG(QBluetoothAddress, QBluetoothAddress(address)),
                                                  Q_ARG(QBluetoothDeviceInfo::Fields, changedFields),
                                                  Q_ARG(qint16, rssi),
                                                  Q_ARG(ManufacturerData, manufacturerData));
                    }
                    return S_OK;
                }
                m_foundLEDevicesMap[address].services = foundServices;
            } else {
                LEAdvertisingInfo info;
                info.services = std::move(serviceUuids);
                info.rssi = rssi;
                m_foundLEDevicesMap.insert(address, info);
            }

            locker.unlock();
        } else
#endif
        {
            if (m_foundLEDevices.contains(address)) {
                if (m_foundLEDevices.value(address) != rssi) {
                    m_foundLEDevices[address] = rssi;
                    changedFields.setFlag(QBluetoothDeviceInfo::Field::RSSI);
                }
                if (!changedFields.testFlag(QBluetoothDeviceInfo::Field::None)) {
                    QMetaObject::invokeMethod(this, "deviceDataChanged", Qt::AutoConnection,
                                              Q_ARG(QBluetoothAddress, QBluetoothAddress(address)),
                                              Q_ARG(QBluetoothDeviceInfo::Fields, changedFields),
                                              Q_ARG(qint16, rssi),
                                              Q_ARG(ManufacturerData, manufacturerData));
                }
                return S_OK;
            }
            m_foundLEDevices.insert(address, rssi);
        }
        leBluetoothInfoFromAddressAsync(address);
        return S_OK;
    }).Get(), &m_leDeviceAddedToken);
    Q_ASSERT_SUCCEEDED(hr);
    hr = m_leWatcher->Start();
    Q_ASSERT_SUCCEEDED(hr);
}

void QWinRTBluetoothDeviceDiscoveryWorker::finishDiscovery()
{
    emit scanFinished();
    stopLEWatcher();
    deleteLater();
}

// "deviceFound" will be emitted at the end of the deviceFromIdOperation callback
void QWinRTBluetoothDeviceDiscoveryWorker::classicBluetoothInfoFromDeviceIdAsync(HSTRING deviceId)
{
    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([deviceId, this]() {
        ComPtr<IAsyncOperation<BluetoothDevice *>> deviceFromIdOperation;
        // on Windows 10 FromIdAsync might ask for device permission. We cannot wait here but have to handle that asynchronously
        HRESULT hr = m_deviceStatics->FromIdAsync(deviceId, &deviceFromIdOperation);
        if (FAILED(hr)) {
            --m_pendingPairedDevices;
            if (!m_pendingPairedDevices
                    && !(requestedModes & QBluetoothDeviceDiscoveryAgent::LowEnergyMethod))
                finishDiscovery();
            qCWarning(QT_BT_WINRT) << "Could not obtain bluetooth device from id";
            return S_OK;
        }
        QPointer<QWinRTBluetoothDeviceDiscoveryWorker> thisPointer(this);
        hr = deviceFromIdOperation->put_Completed(Callback<IAsyncOperationCompletedHandler<BluetoothDevice *>>
                                                  ([thisPointer](IAsyncOperation<BluetoothDevice *> *op, AsyncStatus status)
        {
            if (thisPointer) {
                if (status == Completed)
                    thisPointer->onPairedClassicBluetoothDeviceFoundAsync(op, status);
                --thisPointer->m_pendingPairedDevices;
            }
            return S_OK;
        }).Get());
        if (FAILED(hr)) {
            --m_pendingPairedDevices;
            if (!m_pendingPairedDevices
                    && !(requestedModes & QBluetoothDeviceDiscoveryAgent::LowEnergyMethod))
                finishDiscovery();
            qCWarning(QT_BT_WINRT) << "Could not register device found callback";
            return S_OK;
        }
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

// "deviceFound" will be emitted at the end of the deviceFromIdOperation callback
void QWinRTBluetoothDeviceDiscoveryWorker::leBluetoothInfoFromDeviceIdAsync(HSTRING deviceId)
{
    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([deviceId, this]() {
        ComPtr<IAsyncOperation<BluetoothLEDevice *>> deviceFromIdOperation;
        // on Windows 10 FromIdAsync might ask for device permission. We cannot wait here but have to handle that asynchronously
        HRESULT hr = m_leDeviceStatics->FromIdAsync(deviceId, &deviceFromIdOperation);
        if (FAILED(hr)) {
            --m_pendingPairedDevices;
            qCWarning(QT_BT_WINRT) << "Could not obtain bluetooth device from id";
            return S_OK;
        }
        QPointer<QWinRTBluetoothDeviceDiscoveryWorker> thisPointer(this);
        hr = deviceFromIdOperation->put_Completed(Callback<IAsyncOperationCompletedHandler<BluetoothLEDevice *>>
                                                  ([thisPointer] (IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status)
        {
            if (thisPointer) {
                if (status == Completed)
                    thisPointer->onPairedBluetoothLEDeviceFoundAsync(op, status);
                --thisPointer->m_pendingPairedDevices;
            }
            return S_OK;
        }).Get());
        if (FAILED(hr)) {
            --m_pendingPairedDevices;
            qCWarning(QT_BT_WINRT) << "Could not register device found callback";
            return S_OK;
        }
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

// "deviceFound" will be emitted at the end of the deviceFromAdressOperation callback
void QWinRTBluetoothDeviceDiscoveryWorker::leBluetoothInfoFromAddressAsync(quint64 address)
{
    HRESULT hr;
    hr = QEventDispatcherWinRT::runOnXamlThread([address, this]() {
        ComPtr<IAsyncOperation<BluetoothLEDevice *>> deviceFromAddressOperation;
        // on Windows 10 FromBluetoothAddressAsync might ask for device permission. We cannot wait
        // here but have to handle that asynchronously
        HRESULT hr = m_leDeviceStatics->FromBluetoothAddressAsync(address, &deviceFromAddressOperation);
        if (FAILED(hr)) {
            qCWarning(QT_BT_WINRT) << "Could not obtain bluetooth device from address";
            return S_OK;
        }
        QPointer<QWinRTBluetoothDeviceDiscoveryWorker> thisPointer(this);
        hr = deviceFromAddressOperation->put_Completed(Callback<IAsyncOperationCompletedHandler<BluetoothLEDevice *>>
                                                       ([thisPointer](IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status)
        {
            if (status == Completed && thisPointer)
                thisPointer->onBluetoothLEDeviceFoundAsync(op, status);
            return S_OK;
        }).Get());
        if (FAILED(hr)) {
            qCWarning(QT_BT_WINRT) << "Could not register device found callback";
            return S_OK;
        }
        return S_OK;
    });
    Q_ASSERT_SUCCEEDED(hr);
}

HRESULT QWinRTBluetoothDeviceDiscoveryWorker::onPairedClassicBluetoothDeviceFoundAsync(IAsyncOperation<BluetoothDevice *> *op, AsyncStatus status)
{
    --m_pendingPairedDevices;
    if (status != AsyncStatus::Completed)
        return S_OK;

    ComPtr<IBluetoothDevice> device;
    HRESULT hr = op->GetResults(&device);
    Q_ASSERT_SUCCEEDED(hr);

    if (!device)
        return S_OK;

    UINT64 address;
    HString name;
    ComPtr<IBluetoothClassOfDevice> classOfDevice;
    UINT32 classOfDeviceInt;
    hr = device->get_BluetoothAddress(&address);
    Q_ASSERT_SUCCEEDED(hr);
    hr = device->get_Name(name.GetAddressOf());
    Q_ASSERT_SUCCEEDED(hr);
    const QString btName = QString::fromWCharArray(WindowsGetStringRawBuffer(name.Get(), nullptr));
    hr = device->get_ClassOfDevice(&classOfDevice);
    Q_ASSERT_SUCCEEDED(hr);
    hr = classOfDevice->get_RawValue(&classOfDeviceInt);
    Q_ASSERT_SUCCEEDED(hr);
    IVectorView <Rfcomm::RfcommDeviceService *> *deviceServices;
    hr = device->get_RfcommServices(&deviceServices);
    if (hr == E_ACCESSDENIED) {
        qCWarning(QT_BT_WINRT) << "Could not obtain device services. Please check you have "
                                  "permission to access the device.";
    } else {
        Q_ASSERT_SUCCEEDED(hr);
        uint serviceCount;
        hr = deviceServices->get_Size(&serviceCount);
        Q_ASSERT_SUCCEEDED(hr);
        QVector<QBluetoothUuid> uuids;
        for (uint i = 0; i < serviceCount; ++i) {
            ComPtr<Rfcomm::IRfcommDeviceService> service;
            hr = deviceServices->GetAt(i, &service);
            Q_ASSERT_SUCCEEDED(hr);
            ComPtr<Rfcomm::IRfcommServiceId> id;
            hr = service->get_ServiceId(&id);
            Q_ASSERT_SUCCEEDED(hr);
            GUID uuid;
            hr = id->get_Uuid(&uuid);
            Q_ASSERT_SUCCEEDED(hr);
            uuids.append(QBluetoothUuid(uuid));
        }

        qCDebug(QT_BT_WINRT) << "Discovered BT device: " << QString::number(address) << btName
            << "Num UUIDs" << uuids.count();

        QBluetoothDeviceInfo info(QBluetoothAddress(address), btName, classOfDeviceInt);
        info.setCoreConfigurations(QBluetoothDeviceInfo::BaseRateCoreConfiguration);
        info.setServiceUuids(uuids);
        info.setCached(true);

        QMetaObject::invokeMethod(this, "deviceFound", Qt::AutoConnection,
                                  Q_ARG(QBluetoothDeviceInfo, info));
    }
    if (!m_pendingPairedDevices && !(requestedModes & QBluetoothDeviceDiscoveryAgent::LowEnergyMethod))
        finishDiscovery();
    return S_OK;
}

HRESULT QWinRTBluetoothDeviceDiscoveryWorker::onPairedBluetoothLEDeviceFoundAsync(IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status)
{
    --m_pendingPairedDevices;
    if (status != AsyncStatus::Completed)
        return S_OK;

    ComPtr<IBluetoothLEDevice> device;
    HRESULT hr;
    hr = op->GetResults(&device);
    Q_ASSERT_SUCCEEDED(hr);
#if QT_CONFIG(winrt_btle_no_pairing)
    if (supportsNewLEApi())
        return onBluetoothLEDeviceFound(device);
    else
#endif
        return onBluetoothLEDeviceFound(device, OmitPairingCheck);
}

HRESULT QWinRTBluetoothDeviceDiscoveryWorker::onBluetoothLEDeviceFoundAsync(IAsyncOperation<BluetoothLEDevice *> *op, AsyncStatus status)
{
    if (status != AsyncStatus::Completed)
        return S_OK;

    ComPtr<IBluetoothLEDevice> device;
    HRESULT hr;
    hr = op->GetResults(&device);
    Q_ASSERT_SUCCEEDED(hr);
#if QT_CONFIG(winrt_btle_no_pairing)
    if (supportsNewLEApi())
        return onBluetoothLEDeviceFound(device);
    else
#endif
        return onBluetoothLEDeviceFound(device, PairingCheck::CheckForPairing);
}

HRESULT QWinRTBluetoothDeviceDiscoveryWorker::onBluetoothLEDeviceFound(ComPtr<IBluetoothLEDevice> device, PairingCheck pairingCheck)
{
    if (!device) {
        qCDebug(QT_BT_WINRT) << "onBluetoothLEDeviceFound: No device given";
        return S_OK;
    }

    if (pairingCheck == CheckForPairing) {
        ComPtr<IBluetoothLEDevice2> device2;
        HRESULT hr = device.As(&device2);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<IDeviceInformation> deviceInfo;
        hr = device2->get_DeviceInformation(&deviceInfo);
        Q_ASSERT_SUCCEEDED(hr);
        if (!deviceInfo) {
            qCDebug(QT_BT_WINRT) << "onBluetoothLEDeviceFound: Could not obtain device information";
            return S_OK;
        }
        ComPtr<IDeviceInformation2> deviceInfo2;
        hr = deviceInfo.As(&deviceInfo2);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<IDeviceInformationPairing> pairing;
        hr = deviceInfo2->get_Pairing(&pairing);
        Q_ASSERT_SUCCEEDED(hr);
        boolean isPaired;
        hr = pairing->get_IsPaired(&isPaired);
        Q_ASSERT_SUCCEEDED(hr);
        // We need a paired device in order to be able to obtain its information
        if (!isPaired) {
            ComPtr<IAsyncOperation<DevicePairingResult *>> pairingOp;
            QPointer<QWinRTBluetoothDeviceDiscoveryWorker> tPointer(this);
            hr = pairing.Get()->PairAsync(&pairingOp);
            Q_ASSERT_SUCCEEDED(hr);
            pairingOp->put_Completed(
                Callback<IAsyncOperationCompletedHandler<DevicePairingResult *>>([device, tPointer](IAsyncOperation<DevicePairingResult *> *op, AsyncStatus status) {
                if (!tPointer)
                    return S_OK;

                if (status != AsyncStatus::Completed) {
                    qCDebug(QT_BT_WINRT) << "Could not pair device";
                    return S_OK;
                }

                ComPtr<IDevicePairingResult> result;
                op->GetResults(&result);

                DevicePairingResultStatus pairingStatus;
                result.Get()->get_Status(&pairingStatus);

                if (pairingStatus != DevicePairingResultStatus_Paired) {
                    qCDebug(QT_BT_WINRT) << "Could not pair device";
                    return S_OK;
                }

                tPointer->onBluetoothLEDeviceFound(device, OmitPairingCheck);
                return S_OK;
            }).Get());
            return S_OK;
        }
    }

    UINT64 address;
    HString name;
    HRESULT hr = device->get_BluetoothAddress(&address);
    Q_ASSERT_SUCCEEDED(hr);
    hr = device->get_Name(name.GetAddressOf());
    Q_ASSERT_SUCCEEDED(hr);
    const QString btName = QString::fromWCharArray(WindowsGetStringRawBuffer(name.Get(), nullptr));
    IVectorView <GenericAttributeProfile::GattDeviceService *> *deviceServices;
    hr = device->get_GattServices(&deviceServices);
    Q_ASSERT_SUCCEEDED(hr);
    uint serviceCount;
    hr = deviceServices->get_Size(&serviceCount);
    Q_ASSERT_SUCCEEDED(hr);
    QVector<QBluetoothUuid> uuids;
    for (uint i = 0; i < serviceCount; ++i) {
        ComPtr<GenericAttributeProfile::IGattDeviceService> service;
        hr = deviceServices->GetAt(i, &service);
        Q_ASSERT_SUCCEEDED(hr);
        ComPtr<Rfcomm::IRfcommServiceId> id;
        GUID uuid;
        hr = service->get_Uuid(&uuid);
        Q_ASSERT_SUCCEEDED(hr);
        uuids.append(QBluetoothUuid(uuid));
    }
    const qint16 rssi = m_foundLEDevices.value(address);
    const ManufacturerData manufacturerData = m_foundLEManufacturerData.value(address);

    qCDebug(QT_BT_WINRT) << "Discovered BTLE device: " << QString::number(address) << btName
        << "Num UUIDs" << uuids.count() << "RSSI:" << rssi
        << "Num manufacturer data" << manufacturerData.count();

    QBluetoothDeviceInfo info(QBluetoothAddress(address), btName, 0);
    info.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    info.setServiceUuids(uuids);
    info.setRssi(rssi);
    for (const quint16 key : manufacturerData.keys())
        info.setManufacturerData(key, manufacturerData.value(key));
    info.setCached(true);

    QMetaObject::invokeMethod(this, "deviceFound", Qt::AutoConnection,
                              Q_ARG(QBluetoothDeviceInfo, info));
    return S_OK;
}

#if QT_CONFIG(winrt_btle_no_pairing)
HRESULT QWinRTBluetoothDeviceDiscoveryWorker::onBluetoothLEDeviceFound(ComPtr<IBluetoothLEDevice> device)
{
    if (!device) {
        qCDebug(QT_BT_WINRT) << "onBluetoothLEDeviceFound: No device given";
        return S_OK;
    }

    UINT64 address;
    HString name;
    HRESULT hr = device->get_BluetoothAddress(&address);
    Q_ASSERT_SUCCEEDED(hr);
    hr = device->get_Name(name.GetAddressOf());
    Q_ASSERT_SUCCEEDED(hr);
    const QString btName = QString::fromWCharArray(WindowsGetStringRawBuffer(name.Get(), nullptr));

    ComPtr<IBluetoothLEDevice2> device2;
    hr = device.As(&device2);
    Q_ASSERT_SUCCEEDED(hr);
    ComPtr<IDeviceInformation> deviceInfo;
    hr = device2->get_DeviceInformation(&deviceInfo);
    Q_ASSERT_SUCCEEDED(hr);
    if (!deviceInfo) {
        qCDebug(QT_BT_WINRT) << "onBluetoothLEDeviceFound: Could not obtain device information";
        return S_OK;
    }
    ComPtr<IDeviceInformation2> deviceInfo2;
    hr = deviceInfo.As(&deviceInfo2);
    Q_ASSERT_SUCCEEDED(hr);
    ComPtr<IDeviceInformationPairing> pairing;
    hr = deviceInfo2->get_Pairing(&pairing);
    Q_ASSERT_SUCCEEDED(hr);
    boolean isPaired;
    hr = pairing->get_IsPaired(&isPaired);
    Q_ASSERT_SUCCEEDED(hr);
    QVector<QBluetoothUuid> uuids;

    const LEAdvertisingInfo adInfo = m_foundLEDevicesMap.value(address);
    const qint16 rssi = adInfo.rssi;
    // Use the services obtained from the advertisement data if the device is not paired
    if (!isPaired) {
        uuids = adInfo.services;
    } else {
        IVectorView <GenericAttributeProfile::GattDeviceService *> *deviceServices;
        hr = device->get_GattServices(&deviceServices);
        Q_ASSERT_SUCCEEDED(hr);
        uint serviceCount;
        hr = deviceServices->get_Size(&serviceCount);
        Q_ASSERT_SUCCEEDED(hr);
        for (uint i = 0; i < serviceCount; ++i) {
            ComPtr<GenericAttributeProfile::IGattDeviceService> service;
            hr = deviceServices->GetAt(i, &service);
            Q_ASSERT_SUCCEEDED(hr);
            GUID uuid;
            hr = service->get_Uuid(&uuid);
            Q_ASSERT_SUCCEEDED(hr);
            uuids.append(QBluetoothUuid(uuid));
        }
    }
    const ManufacturerData manufacturerData = m_foundLEManufacturerData.value(address);

    qCDebug(QT_BT_WINRT) << "Discovered BTLE device: " << QString::number(address) << btName
        << "Num UUIDs" << uuids.count() << "RSSI:" << rssi
        << "Num manufacturer data" << manufacturerData.count();

    QBluetoothDeviceInfo info(QBluetoothAddress(address), btName, 0);
    info.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    info.setServiceUuids(uuids);
    info.setRssi(rssi);
    for (quint16 key : manufacturerData.keys())
        info.setManufacturerData(key, manufacturerData.value(key));
    info.setCached(true);

    QMetaObject::invokeMethod(this, "deviceFound", Qt::AutoConnection,
                              Q_ARG(QBluetoothDeviceInfo, info));
    return S_OK;
}
#endif // QT_CONFIG(winrt_btle_no_pairing)

QBluetoothDeviceDiscoveryAgentPrivate::QBluetoothDeviceDiscoveryAgentPrivate(
                const QBluetoothAddress &deviceAdapter,
                QBluetoothDeviceDiscoveryAgent *parent)

    :   lastError(QBluetoothDeviceDiscoveryAgent::NoError),
        lowEnergySearchTimeout(25000),
        q_ptr(parent),
        leScanTimer(0)
{
    Q_UNUSED(deviceAdapter);
}

QBluetoothDeviceDiscoveryAgentPrivate::~QBluetoothDeviceDiscoveryAgentPrivate()
{
    disconnectAndClearWorker();
}

bool QBluetoothDeviceDiscoveryAgentPrivate::isActive() const
{
    return worker;
}

QBluetoothDeviceDiscoveryAgent::DiscoveryMethods QBluetoothDeviceDiscoveryAgent::supportedDiscoveryMethods()
{
    return (ClassicMethod | LowEnergyMethod);
}

void QBluetoothDeviceDiscoveryAgentPrivate::start(QBluetoothDeviceDiscoveryAgent::DiscoveryMethods methods)
{
    if (worker)
        return;

    worker = new QWinRTBluetoothDeviceDiscoveryWorker(methods);
    discoveredDevices.clear();
    connect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::deviceFound,
            this, &QBluetoothDeviceDiscoveryAgentPrivate::registerDevice);
    connect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::deviceDataChanged,
            this, &QBluetoothDeviceDiscoveryAgentPrivate::updateDeviceData);
    connect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::scanFinished,
            this, &QBluetoothDeviceDiscoveryAgentPrivate::onScanFinished);
    worker->start();

    if (lowEnergySearchTimeout > 0 && methods & QBluetoothDeviceDiscoveryAgent::LowEnergyMethod) { // otherwise no timeout and stop() required
        if (!leScanTimer) {
            leScanTimer = new QTimer(this);
            leScanTimer->setSingleShot(true);
        }
        connect(leScanTimer, &QTimer::timeout,
            worker, &QWinRTBluetoothDeviceDiscoveryWorker::finishDiscovery);
        leScanTimer->setInterval(lowEnergySearchTimeout);
        leScanTimer->start();
    }
}

void QBluetoothDeviceDiscoveryAgentPrivate::stop()
{
    Q_Q(QBluetoothDeviceDiscoveryAgent);
    if (worker) {
        worker->stopLEWatcher();
        disconnectAndClearWorker();
        emit q->canceled();
    }
    if (leScanTimer)
        leScanTimer->stop();
}

void QBluetoothDeviceDiscoveryAgentPrivate::registerDevice(const QBluetoothDeviceInfo &info)
{
    Q_Q(QBluetoothDeviceDiscoveryAgent);

    for (QList<QBluetoothDeviceInfo>::iterator iter = discoveredDevices.begin();
        iter != discoveredDevices.end(); ++iter) {
        if (iter->address() == info.address()) {
            qCDebug(QT_BT_WINRT) << "Updating device" << iter->name() << iter->address();
            // merge service uuids
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QVector<QBluetoothUuid> uuids = iter->serviceUuids();
#else
            QList<QBluetoothUuid> uuids = iter->serviceUuids();
#endif
            uuids.append(info.serviceUuids());
            const QSet<QBluetoothUuid> uuidSet(uuids.begin(), uuids.end());
            if (iter->serviceUuids().count() != uuidSet.count())
                iter->setServiceUuids(uuidSet.values().toVector());
            if (iter->coreConfigurations() != info.coreConfigurations())
                iter->setCoreConfigurations(QBluetoothDeviceInfo::BaseRateAndLowEnergyCoreConfiguration);
            return;
        }
    }

    discoveredDevices << info;
    emit q->deviceDiscovered(info);
}

void QBluetoothDeviceDiscoveryAgentPrivate::updateDeviceData(const QBluetoothAddress &address,
                                                             QBluetoothDeviceInfo::Fields fields,
                                                             qint16 rssi,
                                                             ManufacturerData manufacturerData)
{
    if (fields.testFlag(QBluetoothDeviceInfo::Field::None))
        return;

    Q_Q(QBluetoothDeviceDiscoveryAgent);
    for (QList<QBluetoothDeviceInfo>::iterator iter = discoveredDevices.begin();
        iter != discoveredDevices.end(); ++iter) {
        if (iter->address() == address) {
            qCDebug(QT_BT_WINRT) << "Updating data for device" << iter->name() << iter->address();
            if (fields.testFlag(QBluetoothDeviceInfo::Field::RSSI))
                iter->setRssi(rssi);
            if (fields.testFlag(QBluetoothDeviceInfo::Field::ManufacturerData))
                for (quint16 key : manufacturerData.keys())
                    iter->setManufacturerData(key, manufacturerData.value(key));
            emit q->deviceUpdated(*iter, fields);
            return;
        }
    }
}

void QBluetoothDeviceDiscoveryAgentPrivate::onScanFinished()
{
    Q_Q(QBluetoothDeviceDiscoveryAgent);
    disconnectAndClearWorker();
    emit q->finished();
}

void QBluetoothDeviceDiscoveryAgentPrivate::disconnectAndClearWorker()
{
    if (!worker)
        return;

    disconnect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::scanFinished,
               this, &QBluetoothDeviceDiscoveryAgentPrivate::onScanFinished);
    disconnect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::deviceFound,
               this, &QBluetoothDeviceDiscoveryAgentPrivate::registerDevice);
    disconnect(worker, &QWinRTBluetoothDeviceDiscoveryWorker::deviceDataChanged,
               this, &QBluetoothDeviceDiscoveryAgentPrivate::updateDeviceData);
    if (leScanTimer) {
        disconnect(leScanTimer, &QTimer::timeout,
                   worker, &QWinRTBluetoothDeviceDiscoveryWorker::finishDiscovery);
    }
    worker.clear();
}

QT_END_NAMESPACE

#include <qbluetoothdevicediscoveryagent_winrt.moc>
