import AppKit
import AVFoundation
import CoreAudio
import CoreWLAN
import IOBluetooth

struct Failure: Error { let message: String }
func address(_ selector: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
}
func read<T>(_ object: AudioObjectID, _ a: AudioObjectPropertyAddress, _ initial: T) throws -> T {
    var a = a, value = initial, size = UInt32(MemoryLayout<T>.size)
    let code = withUnsafeMutableBytes(of: &value) { AudioObjectGetPropertyData(object, &a, 0, nil, &size, $0.baseAddress!) }
    guard code == noErr else { throw Failure(message: "CoreAudio 属性不可读 (\(code))") }
    return value
}
func writable(_ object: AudioObjectID, _ a: AudioObjectPropertyAddress) -> Bool {
    var a = a, flag: DarwinBoolean = false
    return AudioObjectIsPropertySettable(object, &a, &flag) == noErr && flag.boolValue
}
func write<T>(_ object: AudioObjectID, _ a: AudioObjectPropertyAddress, _ value: T) throws {
    var a = a, value = value
    guard writable(object, a) else { throw Failure(message: "此输出设备不支持软件音量或该属性；在 Mac 调节") }
    let code = withUnsafeBytes(of: &value) { AudioObjectSetPropertyData(object, &a, 0, nil, UInt32(MemoryLayout<T>.size), $0.baseAddress!) }
    guard code == noErr else { throw Failure(message: "CoreAudio 操作失败 (\(code))") }
}
func stringProperty(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String {
    var a = address(selector), value: Unmanaged<CFString>? = nil, size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(object, &a, 0, nil, &size, &value) == noErr, let value else { return "" }
    return value.takeRetainedValue() as String
}
func audioDevices() -> [AudioObjectID] {
    var a = address(kAudioHardwarePropertyDevices), size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size) == noErr else { return [] }
    var devices = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &devices) == noErr else { return [] }
    return devices.filter { device in
        var streams = address(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput), bytes: UInt32 = 0
        return AudioObjectGetPropertyDataSize(device, &streams, 0, nil, &bytes) == noErr && bytes > 0
    }
}
func output() throws -> AudioObjectID {
    try read(AudioObjectID(kAudioObjectSystemObject), address(kAudioHardwarePropertyDefaultOutputDevice), AudioObjectID(0))
}
func volumeElements(_ device: AudioObjectID) -> [AudioObjectPropertyElement] {
    let main = address(kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput)
    if writable(device, main) { return [kAudioObjectPropertyElementMain] }
    // Stereo devices often expose individual writable channels instead of master.
    let channels: [AudioObjectPropertyElement] = [1, 2]
    return channels.filter { writable(device, address(kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, $0)) }
}
func actualVolume(_ device: AudioObjectID) -> Float32? {
    let elements = volumeElements(device)
    let values = elements.compactMap { try? read(device, address(kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, $0), Float32(0)) }
    return values.isEmpty ? nil : values.reduce(0,+) / Float32(values.count) * 100
}
struct AudioReading { let volume: Float32?; let mute: UInt32? }
var confirmedAudio: [String: AudioReading] = [:]
func bluetoothOutputIdle(_ device: AudioObjectID) -> Bool {
    let transport: UInt32 = (try? read(device,address(kAudioDevicePropertyTransportType),0)) ?? 0
    guard transport == kAudioDeviceTransportTypeBluetooth || transport == kAudioDeviceTransportTypeBluetoothLE else { return false }
    let running: UInt32 = (try? read(device,address(kAudioDevicePropertyDeviceIsRunningSomewhere),0)) ?? 0
    return running == 0
}
func rememberAudio(_ device: AudioObjectID) {
    let uid = stringProperty(device,kAudioDevicePropertyDeviceUID)
    guard !uid.isEmpty, !bluetoothOutputIdle(device) else { return }
    confirmedAudio[uid] = AudioReading(volume:actualVolume(device),mute:try? read(device,address(kAudioDevicePropertyMute,kAudioDevicePropertyScopeOutput),UInt32(0)))
}
func waitForAudioState(_ matches: () -> Bool) -> Bool {
    // HAL writes may complete asynchronously, especially for Bluetooth output.
    // Deliver main-loop callbacks and confirm the actual value before success.
    let deadline = Date(timeIntervalSinceNow: 3)
    repeat {
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.05))
        if matches() { return true }
    } while Date() < deadline
    return false
}
final class SilentOutputSession {
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()

    init(device: AudioObjectID) throws {
        guard (try? output()) == device else { throw Failure(message:"输出设备已变化；刷新后重新调节") }
        engine.attach(player)
        let format = engine.outputNode.outputFormat(forBus:0)
        guard format.sampleRate > 0, format.channelCount > 0,
              let silence = AVAudioPCMBuffer(pcmFormat:format,frameCapacity:1024) else {
            throw Failure(message:"耳机音频通道未就绪；重新连接后再调节")
        }
        engine.connect(player,to:engine.mainMixerNode,format:format)
        silence.frameLength = silence.frameCapacity
        for buffer in UnsafeMutableAudioBufferListPointer(silence.mutableAudioBufferList) {
            if let data = buffer.mData { memset(data,0,Int(buffer.mDataByteSize)) }
        }
        player.volume = 0
        player.scheduleBuffer(silence,at:nil,options:.loops)
        do { try engine.start() }
        catch {
            stop()
            throw Failure(message:"耳机音频通道未就绪；重新连接后再调节")
        }
        player.play()
        // An idle Bluetooth transport can report writable 50% placeholders and
        // acknowledge writes without applying them. Start only a zero-filled
        // output stream so the transport exposes its real controls, then retain
        // it through the readback. No input node or recording is used.
        RunLoop.current.run(until:Date(timeIntervalSinceNow:1))
        guard (try? output()) == device else {
            stop()
            throw Failure(message:"输出设备已变化；刷新后重新调节")
        }
    }
    func stop() { player.stop(); engine.stop() }
    deinit { stop() }
}
func prepareBluetoothOutput(_ device: AudioObjectID) throws -> SilentOutputSession? {
    return bluetoothOutputIdle(device) ? try SilentOutputSession(device:device) : nil
}
func runNetwork(_ arguments: [String]) throws -> (Int32, String) {
    let process = Process(), pipe = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/sbin/networksetup")
    process.arguments = arguments; process.standardOutput = pipe; process.standardError = pipe
    try process.run()
    let data = pipe.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
    return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
}
func currentNetwork(_ interface: CWInterface) -> String? {
    if let current=interface.ssid() { return current }
    guard let name=interface.interfaceName, let (code,result)=try? runNetwork(["-getairportnetwork",name]), code==0, result.hasPrefix("Current Wi-Fi Network: ") else { return nil }
    return String(result.dropFirst("Current Wi-Fi Network: ".count)).trimmingCharacters(in:.whitespacesAndNewlines)
}
func savedNetworks(_ interface: CWInterface) -> [String] {
    if let profiles = interface.configuration()?.networkProfiles.array as? [CWNetworkProfile] {
        let names = profiles.compactMap { $0.ssid }; if !names.isEmpty { return names }
    }
    guard let name = interface.interfaceName, let (_, result) = try? runNetwork(["-listpreferredwirelessnetworks",name]) else { return [] }
    return result.split(separator: "\n").dropFirst().map { String($0).trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
}
func paired() -> [IOBluetoothDevice] {
    var seen = Set<String>()
    return (IOBluetoothDevice.pairedDevices() as? [IOBluetoothDevice] ?? []).filter {
        guard let key = $0.addressString, !key.isEmpty else { return false }
        return seen.insert(key).inserted
    }
}
func waitForConnection(_ device: IOBluetoothDevice, connected: Bool) -> Bool {
    let deadline=Date(timeIntervalSinceNow:5)
    // IOBluetooth updates its device objects through the main run loop.
    // Keep delivering those callbacks while waiting for the actual state.
    repeat {
        RunLoop.current.run(until:Date(timeIntervalSinceNow:0.05))
        if device.isConnected()==connected { return true }
    } while Date()<deadline
    return false
}
func state(_ surface: String = "all") -> [[String: Any]] {
    var rows: [[String: Any]] = []
    if surface == "all" || surface == "audio" {
    let devices = audioDevices()
    let available = Set(devices.map { stringProperty($0,kAudioDevicePropertyDeviceUID) })
    confirmedAudio = confirmedAudio.filter { available.contains($0.key) }
    if let current = try? output() {
        let idle = bluetoothOutputIdle(current)
        if !idle { rememberAudio(current) }
        let cached = confirmedAudio[stringProperty(current,kAudioDevicePropertyDeviceUID)]
        let volume = idle ? cached?.volume : actualVolume(current)
        let mute: UInt32? = idle ? cached?.mute : try? read(current,address(kAudioDevicePropertyMute,kAudioDevicePropertyScopeOutput),UInt32(0))
        let supported = !volumeElements(current).isEmpty
        let reason = !supported ? "此输出设备不支持软件音量，请在设备上调节" : idle ? (cached == nil ? "耳机待机；调节后确认状态" : "耳机待机；显示上次确认状态") : ""
        rows.append(["kind":"status","state":idle ? (cached == nil ? "not_provided" : "cached") : "fresh","volume":volume.map { Double($0) } ?? NSNull(),
                     "muted":mute.map { $0 != 0 } ?? NSNull(),"volume_supported":supported,
                     "mute_supported":writable(current,address(kAudioDevicePropertyMute,kAudioDevicePropertyScopeOutput)),
                     "reason":reason])
        for device in devices {
            rows.append(["kind":"audio","native_id":stringProperty(device,kAudioDevicePropertyDeviceUID),
                         "name":stringProperty(device,kAudioObjectPropertyName),"selected":device == current])
        }
    } else { rows.append(["kind":"status","state":"error","reason":"CoreAudio 输出不可用，请在 Mac 系统设置检查"]) }
    }
    if surface == "all" || surface == "bluetooth" {
    let devices = paired()
    for device in devices.filter({ $0.isConnected() }) + devices.filter({ !$0.isConnected() }) {
        rows.append(["kind":"bluetooth","native_id":device.addressString ?? "", "name":device.name ?? "蓝牙设备", "connected":device.isConnected()])
    }
    if paired().isEmpty { rows.append(["kind":"permission","name":"蓝牙","reason":"没有可读取的已配对设备；在 Mac 配对或授权","settings":"bluetooth"]) }
    }
    if surface == "all" || surface == "wifi" {
    if let interface = CWWiFiClient.shared().interface() {
        let current = currentNetwork(interface)
        for name in savedNetworks(interface) {
            rows.append(["kind":"wifi","native_id":name,"name":name,"selected":current == name])
        }
        if current == nil { rows.append(["kind":"permission","name":"Wi-Fi 状态","reason":"当前网络未提供；检查 Wi-Fi 与定位授权","settings":"wifi"]) }
    } else { rows.append(["kind":"permission","name":"Wi-Fi","reason":"Wi-Fi 接口不可用；在 Mac 设置检查","settings":"wifi"]) }
    }
    return rows
}
var windows: [NSWindow] = []
var sharingPickers: [NSSharingServicePicker] = []
func showSharing(_ path: String) throws -> [String: Any] {
    guard FileManager.default.fileExists(atPath:path) else { throw Failure(message:"文件已移动或删除，请重新选择") }
    let window = NSWindow(contentRect:NSRect(x:0,y:0,width:380,height:150),styleMask:[.titled,.closable],backing:.buffered,defer:false)
    window.title = "ESP32 文件分享"; window.isReleasedWhenClosed = false
    let label = NSTextField(labelWithString:"在 Mac 选择隔空投送及接收者")
    label.frame = NSRect(x:24,y:74,width:330,height:40); window.contentView?.addSubview(label)
    window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps:true); windows.append(window)
    let picker = NSSharingServicePicker(items:[URL(fileURLWithPath:path)])
    sharingPickers.append(picker)
    if sharingPickers.count>8 { sharingPickers.removeFirst() }
    if windows.count>8 { windows.removeFirst().close() }
    picker.show(relativeTo:NSRect(x:30,y:40,width:320,height:28),of:window.contentView!,preferredEdge:.minY)
    // The native panel owns recipient selection and completion. Do not claim a send.
    return ["state":"unconfirmed","reason":"分享面板已打开；在 Mac 选择接收者并完成发送"]
}
func handle(_ request: [String: Any]) throws -> [String: Any] {
    guard let action = request["action"] as? String else { throw Failure(message:"缺少固定动作") }
    switch action {
    case "state": return ["state":"succeeded","items":state(request["surface"] as? String ?? "all")]
    case "volume_set":
        guard let value = request["value"] as? Double, value.isFinite, (0...100).contains(value) else { throw Failure(message:"音量范围无效") }
        let device = try output()
        let silence = try prepareBluetoothOutput(device)
        defer { silence?.stop() }
        let elements = volumeElements(device)
        guard !elements.isEmpty else { throw Failure(message:"此输出设备不支持软件音量") }
        for element in elements { try write(device,address(kAudioDevicePropertyVolumeScalar,kAudioDevicePropertyScopeOutput,element),Float32(value/100)) }
        guard waitForAudioState({ (try? output()) == device && actualVolume(device).map { abs(Double($0)-value)<2 } == true }) else {
            return ["state":"unconfirmed","reason":(try? output()) == device ? "音量未生效；检查耳机连接或在 Mac 调节" : "输出设备已变化；刷新后重新调节音量"]
        }
        rememberAudio(device)
    case "mute_set":
        guard let value = request["value"] as? Bool else { throw Failure(message:"静音值无效") }
        let device = try output()
        let silence = try prepareBluetoothOutput(device)
        defer { silence?.stop() }
        try write(device,address(kAudioDevicePropertyMute,kAudioDevicePropertyScopeOutput),UInt32(value ? 1:0))
        guard waitForAudioState({
            guard (try? output()) == device,
                  let actual: UInt32 = try? read(device,address(kAudioDevicePropertyMute,kAudioDevicePropertyScopeOutput),0) else { return false }
            return (actual != 0) == value
        }) else { return ["state":"unconfirmed","reason":"静音未生效；检查当前输出设备"] }
        rememberAudio(device)
    case "audio_select":
        guard let target = request["target"] as? String, let device = audioDevices().first(where:{stringProperty($0,kAudioDevicePropertyDeviceUID)==target}) else { throw Failure(message:"输出设备已不可用") }
        try write(AudioObjectID(kAudioObjectSystemObject),address(kAudioHardwarePropertyDefaultOutputDevice),device)
        guard waitForAudioState({ (try? output()) == device }) else { return ["state":"unconfirmed","reason":"输出切换未确认；检查设备是否仍可用"] }
    case "bluetooth_connect", "bluetooth_disconnect":
        guard let target = request["target"] as? String, let device = paired().first(where:{$0.addressString==target}) else { throw Failure(message:"只支持已配对设备；请先在 Mac 配对") }
        if device.isConnected() == (action == "bluetooth_connect") { return ["state":"succeeded","reason":"已读取实际连接状态"] }
        let code = action == "bluetooth_connect" ? device.openConnection(nil,withPageTimeout:0x2000,authenticationRequired:true):device.closeConnection()
        guard code == kIOReturnSuccess else {
            if code == kIOReturnTimeout { throw Failure(message:"耳机连接超时；确认已开机且未被其他设备占用") }
            if code == kIOReturnNotPermitted || code == kIOReturnNotPrivileged { throw Failure(message:"蓝牙权限不足；在 Mac 系统设置检查授权") }
            if code == kIOReturnNotReady || code == kIOReturnNoDevice || code == kIOReturnOffline { throw Failure(message:"蓝牙设备未就绪；确认已开机并靠近 Mac") }
            throw Failure(message:"蓝牙操作失败；在 Mac 检查设备 (\(code))")
        }
        guard waitForConnection(device,connected:action == "bluetooth_connect") else { return ["state":"unconfirmed","reason":"蓝牙连接结果尚未确认；刷新状态"] }
    case "wifi_select":
        guard let target = request["target"] as? String, let interface = CWWiFiClient.shared().interface(), let name = interface.interfaceName, savedNetworks(interface).contains(target) else { throw Failure(message:"仅支持已保存 Wi-Fi；在 Mac 设置添加网络") }
        let (code, _) = try runNetwork(["-setairportnetwork",name,target])
        guard code == 0 else { throw Failure(message:"网络切换失败；密码或认证需在 Mac Wi-Fi 设置完成") }
        guard currentNetwork(interface) == target else { return ["state":"unconfirmed","reason":"网络切换提交；等待重连后刷新实际状态"] }
    case "airdrop_open":
        guard NSWorkspace.shared.open(URL(fileURLWithPath:"/System/Library/CoreServices/Finder.app/Contents/Applications/AirDrop.app")) else { throw Failure(message:"无法打开隔空投送窗口") }
    case "settings_open":
        let entries=["audio":"x-apple.systempreferences:com.apple.Sound-Settings.extension", "bluetooth":"x-apple.systempreferences:com.apple.BluetoothSettings", "wifi":"x-apple.systempreferences:com.apple.wifi-settings-extension", "privacy":"x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices"]
        guard let target=request["target"] as? String, let value=entries[target], let url=URL(string:value), NSWorkspace.shared.open(url) else { throw Failure(message:"无法打开系统设置") }
    case "file_pick":
        let panel = NSOpenPanel(); panel.canChooseFiles=true; panel.canChooseDirectories=false; panel.allowsMultipleSelection=false
        NSApp.activate(ignoringOtherApps:true)
        guard panel.runModal() == .OK, let url = panel.url else { return ["state":"failed","reason":"已取消选择"] }
        return ["state":"succeeded","path":url.path]
    case "file_share":
        guard let path = request["path"] as? String else { throw Failure(message:"文件引用无效") }
        return try showSharing(path)
    case "details_open":
        guard let value = request["url"] as? String, let url = URL(string:value), ["http","https"].contains(url.scheme ?? ""), url.user==nil, url.password==nil else { throw Failure(message:"详情地址无效") }
        guard NSWorkspace.shared.open(url) else { throw Failure(message:"无法打开详情") }
    default: throw Failure(message:"不支持该动作")
    }
    return ["state":"succeeded","reason":"已读取实际结果"]
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
DispatchQueue.global().async {
    while let line = readLine() {
        guard line.utf8.count<=8192, let data=line.data(using:.utf8), let request=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] else { continue }
        DispatchQueue.main.async {
            var result: [String:Any]
            do { result = try handle(request) }
            catch let error as Failure { result=["state":"failed","reason":error.message] }
            catch { result=["state":"failed","reason":"Mac 原生操作失败；检查系统设置"] }
            result["id"] = request["id"]
            if let data=try? JSONSerialization.data(withJSONObject:result,options:[.sortedKeys]), var value=String(data:data,encoding:.utf8) {
                value += "\n"; FileHandle.standardOutput.write(value.data(using:.utf8)!)
            }
        }
    }
    DispatchQueue.main.async { app.terminate(nil) }
}
app.run()
