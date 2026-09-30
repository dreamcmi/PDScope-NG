import Cocoa
import FlutterMacOS

/// Flutter method-channel bridge and native file-drop target for the macOS shell.
final class ShellBridge {
  static let shared = ShellBridge()

  private var channel: FlutterMethodChannel?
  private weak var window: NSWindow?
  private var pendingPaths: [String] = []
  private var dartReady = false
  private var menuItems: [String: NSMenuItem] = [:]

  private init() {}

  func attach(to controller: FlutterViewController, window: NSWindow) {
    self.window = window
    guard channel == nil else { return }

    let methodChannel = FlutterMethodChannel(
      name: "pdscope/shell",
      binaryMessenger: controller.engine.binaryMessenger
    )
    channel = methodChannel
    methodChannel.setMethodCallHandler { [weak self] call, result in
      guard let self else {
        result(FlutterError(code: "shell_unavailable", message: "The native shell is unavailable.", details: nil))
        return
      }

      switch call.method {
      case "ready":
        self.dartReady = true
        self.flushPendingPaths()
        result(nil)
      case "shellState":
        if let state = call.arguments as? [String: Any] {
          self.apply(state: state)
        }
        result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
  }

  func receive(paths candidates: [String]) {
    let paths = candidates.compactMap(openablePath)
    guard !paths.isEmpty else { return }

    guard dartReady, channel != nil else {
      pendingPaths.append(contentsOf: paths)
      return
    }
    channel?.invokeMethod("openFiles", arguments: paths)
  }

  func sendCommand(_ command: String) {
    channel?.invokeMethod("command", arguments: command)
  }

  func apply(state: [String: Any]) {
    let hasDocument = state["hasDoc"] as? Bool ?? false
    ["closeCurrent", "closeAll", "exportCsv", "exportJson", "search"].forEach {
      menuItems[$0]?.isEnabled = hasDocument
    }

    menuItems["toggleFilters"]?.state = (state["filters"] as? Bool ?? true) ? .on : .off
    menuItems["toggleDetail"]?.state = (state["detail"] as? Bool ?? true) ? .on : .off

    let title = (state["title"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "PDScope"
    window?.title = title
  }

  func register(menuItem: NSMenuItem, for command: String) {
    menuItems[command] = menuItem
  }

  private func flushPendingPaths() {
    guard !pendingPaths.isEmpty else { return }
    let paths = pendingPaths
    pendingPaths.removeAll(keepingCapacity: false)
    channel?.invokeMethod("openFiles", arguments: paths)
  }

  private func openablePath(_ candidate: String) -> String? {
    guard !candidate.isEmpty, !candidate.hasPrefix("-") else { return nil }

    let url: URL
    if candidate.hasPrefix("file://"), let fileURL = URL(string: candidate), fileURL.isFileURL {
      url = fileURL
    } else {
      let currentDirectory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
      url = URL(fileURLWithPath: candidate, relativeTo: currentDirectory)
    }

    let absoluteURL = url.standardizedFileURL
    guard absoluteURL.isFileURL else { return nil }
    var isDirectory: ObjCBool = false
    guard FileManager.default.fileExists(atPath: absoluteURL.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
      return nil
    }
    return absoluteURL.path
  }
}

/// Host view accepting file URLs while Flutter's child view remains interactive.
final class PDScopeDropView: NSView {
  var onFilesDropped: (([String]) -> Void)?

  override init(frame frameRect: NSRect) {
    super.init(frame: frameRect)
    registerForDraggedTypes([.fileURL])
  }

  required init?(coder: NSCoder) {
    super.init(coder: coder)
    registerForDraggedTypes([.fileURL])
  }

  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
    sender.draggingPasteboard.canReadObject(
      forClasses: [NSURL.self],
      options: [.urlReadingFileURLsOnly: true]
    ) ? .copy : []
  }

  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    let objects = sender.draggingPasteboard.readObjects(
      forClasses: [NSURL.self],
      options: [.urlReadingFileURLsOnly: true]
    ) as? [NSURL] ?? []
    let paths = objects.map { ($0 as URL).path }
    guard !paths.isEmpty else { return false }
    onFilesDropped?(paths)
    return true
  }
}
