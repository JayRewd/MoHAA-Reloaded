// The app icon: an SVG's artwork in macOS's icon shape (Apple's template: an 824/1024 rounded square, centered,
// corner radius 185.4/824, transparent outside), every size of an .iconset, for iconutil.
//   xcrun swiftc -O -o build.noindex/makeicon port/makeicon.swift
//   build.noindex/makeicon misc/macos/openmohaa-metal.svg build.noindex/icon.iconset
//   iconutil -c icns -o misc/macos/openmohaa-metal.icns build.noindex/icon.iconset
// The artwork is the user's (pstngh/moh-icons, mohaa-transparent.svg, byte for byte: its content credentials
// sign the file): the gold star with no background, which macOS 26 puts on a plate of its own.
import AppKit

let args = CommandLine.arguments
guard args.count == 3, let art = NSImage(contentsOfFile: args[1]) else {
	print("usage: makeicon <artwork.svg> <out.iconset>")
	exit(1)
}
try! FileManager.default.createDirectory(atPath: args[2], withIntermediateDirectories: true)

for (points, scale) in [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)] {
	let size = CGFloat(points * scale)
	let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
	                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
	                           bytesPerRow: 0, bitsPerPixel: 0)!
	NSGraphicsContext.saveGraphicsState()
	NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
	let side = size * 824 / 1024, origin = (size - side) / 2
	let shape = NSRect(x: origin, y: origin, width: side, height: side)
	NSBezierPath(roundedRect: shape, xRadius: side * 185.4 / 824, yRadius: side * 185.4 / 824).addClip()
	art.draw(in: shape, from: .zero, operation: .sourceOver, fraction: 1)
	NSGraphicsContext.restoreGraphicsState()
	let name = "icon_\(points)x\(points)" + (scale == 2 ? "@2x" : "") + ".png"
	try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2] + "/" + name))
}
