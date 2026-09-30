import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let containerController = NSViewController()

    let initialContentSize = NSSize(width: 1280, height: 800)
    let dropView = PDScopeDropView(
      frame: self.contentView?.bounds ?? NSRect(origin: .zero, size: initialContentSize)
    )
    containerController.view = dropView
    containerController.addChild(flutterViewController)
    dropView.autoresizingMask = [.width, .height]
    dropView.onFilesDropped = { paths in
      ShellBridge.shared.receive(paths: paths)
    }

    let flutterView = flutterViewController.view
    flutterView.frame = dropView.bounds
    flutterView.autoresizingMask = [.width, .height]
    dropView.addSubview(flutterView)
    // The parent controller retains Flutter's controller and engine for the
    // lifetime of the window, while its root view accepts file drops.
    self.contentViewController = containerController

    self.minSize = NSSize(width: 900, height: 600)
    self.setFrame(NSRect(origin: self.frame.origin, size: initialContentSize), display: true)
    self.title = "PDScope"

    RegisterGeneratedPlugins(registry: flutterViewController)
    ShellBridge.shared.attach(to: flutterViewController, window: self)

    super.awakeFromNib()
  }
}
