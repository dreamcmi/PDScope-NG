import Cocoa
import FlutterMacOS

@main
class AppDelegate: FlutterAppDelegate {
  override func applicationWillFinishLaunching(_ notification: Notification) {
    // FlutterAppDelegate implements willFinishLaunching, but not the optional
    // didFinishLaunching callback. Calling the latter on super aborts setup.
    super.applicationWillFinishLaunching(notification)
    installNativeMenu()

    // Direct launches can include capture paths as process arguments. Cache them
    // until the Flutter method channel says it is ready.
    ShellBridge.shared.receive(paths: Array(ProcessInfo.processInfo.arguments.dropFirst()))
  }

  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true
  }

  override func application(_ sender: NSApplication, openFiles filenames: [String]) {
    ShellBridge.shared.receive(paths: filenames)
    sender.reply(toOpenOrPrint: .success)
  }

  override func application(_ sender: NSApplication, openFile filename: String) -> Bool {
    ShellBridge.shared.receive(paths: [filename])
    return true
  }

  override func application(_ application: NSApplication, open urls: [URL]) {
    ShellBridge.shared.receive(paths: urls.filter(\.isFileURL).map(\.path))
    let otherURLs = urls.filter { !$0.isFileURL }
    if !otherURLs.isEmpty {
      super.application(application, open: otherURLs)
    }
  }

  @objc private func forwardMenuCommand(_ sender: NSMenuItem) {
    guard let command = sender.representedObject as? String else { return }
    ShellBridge.shared.sendCommand(command)
  }

  private func installNativeMenu() {
    let mainMenu = NSMenu(title: "PDScope")

    let appMenuItem = NSMenuItem(title: "PDScope", action: nil, keyEquivalent: "")
    let appMenu = NSMenu(title: "PDScope")
    appMenu.addItem(commandItem("关于 PDScope", command: "about"))
    appMenu.addItem(.separator())

    let servicesMenu = NSMenu(title: "服务")
    NSApp.servicesMenu = servicesMenu
    let servicesItem = NSMenuItem(title: "服务", action: nil, keyEquivalent: "")
    servicesItem.submenu = servicesMenu
    appMenu.addItem(servicesItem)
    appMenu.addItem(.separator())

    appMenu.addItem(systemItem("隐藏 PDScope", action: #selector(NSApplication.hide(_:)), key: "h"))
    appMenu.addItem(systemItem("隐藏其他", action: #selector(NSApplication.hideOtherApplications(_:)), key: "h", modifiers: [.command, .option]))
    appMenu.addItem(systemItem("显示全部", action: #selector(NSApplication.unhideAllApplications(_:)), key: ""))
    appMenu.addItem(.separator())
    appMenu.addItem(systemItem("退出 PDScope", action: #selector(NSApplication.terminate(_:)), key: "q"))
    appMenuItem.submenu = appMenu
    mainMenu.addItem(appMenuItem)

    let fileMenuItem = NSMenuItem(title: "文件", action: nil, keyEquivalent: "")
    let fileMenu = NSMenu(title: "文件")
    fileMenu.autoenablesItems = false
    fileMenu.addItem(commandItem("打开抓包…", command: "openFile", key: "o"))
    fileMenu.addItem(commandItem("连接设备…", command: "connectDevice", key: "d"))
    fileMenu.addItem(commandItem("关闭当前标签", command: "closeCurrent", key: "w"))
    fileMenu.addItem(commandItem("关闭全部抓包", command: "closeAll"))
    fileMenu.addItem(.separator())
    fileMenu.addItem(commandItem("导出 CSV（当前筛选结果）", command: "exportCsv"))
    fileMenu.addItem(commandItem("导出 JSON（全部报文）", command: "exportJson"))
    fileMenuItem.submenu = fileMenu
    mainMenu.addItem(fileMenuItem)

    // Keep AppKit's responder-chain editing actions available, including in
    // native open/save panels and Flutter text fields.
    let editMenuItem = NSMenuItem(title: "编辑", action: nil, keyEquivalent: "")
    let editMenu = NSMenu(title: "编辑")
    editMenu.addItem(systemItem("撤销", action: Selector(("undo:")), key: "z", target: nil))
    editMenu.addItem(systemItem("重做", action: Selector(("redo:")), key: "z", modifiers: [.command, .shift], target: nil))
    editMenu.addItem(.separator())
    editMenu.addItem(systemItem("剪切", action: #selector(NSText.cut(_:)), key: "x", target: nil))
    editMenu.addItem(systemItem("复制", action: #selector(NSText.copy(_:)), key: "c", target: nil))
    editMenu.addItem(systemItem("粘贴", action: #selector(NSText.paste(_:)), key: "v", target: nil))
    editMenu.addItem(systemItem("全选", action: #selector(NSText.selectAll(_:)), key: "a", target: nil))
    editMenuItem.submenu = editMenu
    mainMenu.addItem(editMenuItem)

    let viewMenuItem = NSMenuItem(title: "视图", action: nil, keyEquivalent: "")
    let viewMenu = NSMenu(title: "视图")
    viewMenu.autoenablesItems = false
    viewMenu.addItem(commandItem("搜索报文", command: "search", key: "f"))
    viewMenu.addItem(commandItem("切换明暗主题", command: "toggleTheme"))
    viewMenu.addItem(commandItem("紧凑 / 舒适行高", command: "toggleDense"))
    viewMenu.addItem(.separator())
    viewMenu.addItem(commandItem("显示筛选栏", command: "toggleFilters"))
    viewMenu.addItem(commandItem("显示详情面板", command: "toggleDetail"))
    viewMenuItem.submenu = viewMenu
    mainMenu.addItem(viewMenuItem)

    let windowMenuItem = NSMenuItem(title: "窗口", action: nil, keyEquivalent: "")
    let windowMenu = NSMenu(title: "窗口")
    windowMenu.addItem(systemItem("最小化", action: #selector(NSWindow.performMiniaturize(_:)), key: "m", target: nil))
    windowMenu.addItem(systemItem("缩放", action: #selector(NSWindow.performZoom(_:)), key: "", target: nil))
    windowMenu.addItem(.separator())
    windowMenu.addItem(systemItem("将所有窗口置于最前", action: #selector(NSApplication.arrangeInFront(_:)), key: ""))
    windowMenuItem.submenu = windowMenu
    mainMenu.addItem(windowMenuItem)
    NSApp.windowsMenu = windowMenu

    let helpMenuItem = NSMenuItem(title: "帮助", action: nil, keyEquivalent: "")
    helpMenuItem.submenu = NSMenu(title: "帮助")
    mainMenu.addItem(helpMenuItem)
    NSApp.mainMenu = mainMenu
  }

  private func commandItem(_ title: String, command: String, key: String = "") -> NSMenuItem {
    let modifiers: NSEvent.ModifierFlags = key.isEmpty ? [] : [.command]
    let item = NSMenuItem(title: title, action: #selector(forwardMenuCommand(_:)), keyEquivalent: key)
    item.target = self
    item.representedObject = command
    item.keyEquivalentModifierMask = modifiers
    ShellBridge.shared.register(menuItem: item, for: command)
    return item
  }

  private func systemItem(
    _ title: String,
    action: Selector,
    key: String,
    modifiers: NSEvent.ModifierFlags = [.command],
    target: AnyObject? = NSApp
  ) -> NSMenuItem {
    let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
    item.target = target
    item.keyEquivalentModifierMask = key.isEmpty ? [] : modifiers
    return item
  }
}
