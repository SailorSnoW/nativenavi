// The live stick test under a row with meta test=sticks (the DM_PAD_DEADZONE row): both sticks of the first connected controller,
// the dead zone the row sets, and whether the game receives input. The host applies the same round dead zone (dm_input.h dm_stick_apply).
import AppKit
import GameController
import SwiftUI

final class StickMonitor: ObservableObject {   // polled on the main queue: SwiftUI reads the published values there
    @Published var name: String?
    @Published var left = CGPoint.zero    // raw stick, -1...1, x right, y up (GameController's axes)
    @Published var right = CGPoint.zero
    private var timer: Timer?

    init() {
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
            DispatchQueue.main.async { self?.poll() }
        }
    }

    private func poll() {
        guard let c = GCController.controllers().first(where: { $0.extendedGamepad != nil }), let g = c.extendedGamepad else {
            name = nil; left = .zero; right = .zero
            return
        }
        name = c.vendorName
        left = CGPoint(x: CGFloat(g.leftThumbstick.xAxis.value), y: CGFloat(g.leftThumbstick.yAxis.value))
        right = CGPoint(x: CGFloat(g.rightThumbstick.xAxis.value), y: CGFloat(g.rightThumbstick.yAxis.value))
    }
}

struct StickTestView: View {
    let setting: Setting
    @ObservedObject var m: LauncherModel
    @StateObject private var monitor = StickMonitor()

    private var deadzone: Double { Double(m.values[setting.env] ?? setting.def) ?? 0 }   // a share of the full deflection, as the host reads it

    private func verdict(_ p: CGPoint) -> String {
        let r = hypot(p.x, p.y)
        if r <= deadzone { return "centred, no input" }
        return "emitting \(Int((r - deadzone) / (1 - deadzone) * 100)) %"
    }

    var body: some View {
        Section {
            VStack(spacing: 6) {
                HStack(spacing: 28) {
                    StickPad(title: "Left stick", raw: monitor.left, deadzone: deadzone)
                    StickPad(title: "Right stick", raw: monitor.right, deadzone: deadzone)
                }
                Text("Left: \(verdict(monitor.left)) · Right: \(verdict(monitor.right))")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding(.vertical, 4)
        } footer: {
            let how = "Dot: raw stick · ring: dead zone · green: input the game receives."
            if let name = monitor.name { Text("\(name). \(how)") }
            else { Text("Connect a controller to test it. \(how)") }
        }
    }
}

private struct StickPad: View {
    let title: String
    let raw: CGPoint
    let deadzone: Double

    var body: some View {
        VStack(spacing: 4) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Canvas { ctx, size in
                let c = CGPoint(x: size.width / 2, y: size.height / 2)
                let half = min(size.width, size.height) / 2 - 2
                ctx.stroke(Path { $0.addRect(CGRect(x: c.x - half, y: c.y - half, width: 2 * half, height: 2 * half)) },
                           with: .color(.secondary), lineWidth: 1)
                ctx.stroke(Path { p in   // crosshair
                    p.move(to: CGPoint(x: c.x - half, y: c.y)); p.addLine(to: CGPoint(x: c.x + half, y: c.y))
                    p.move(to: CGPoint(x: c.x, y: c.y - half)); p.addLine(to: CGPoint(x: c.x, y: c.y + half))
                }, with: .color(.secondary.opacity(0.4)), lineWidth: 1)
                let dz = max(0, min(deadzone, 1)) * half
                ctx.stroke(Path { $0.addEllipse(in: CGRect(x: c.x - dz, y: c.y - dz, width: 2 * dz, height: 2 * dz)) },
                           with: .color(.accentColor), lineWidth: 1.5)
                let r = hypot(raw.x, raw.y)
                let pos = CGPoint(x: c.x + CGFloat(raw.x) * half, y: c.y - CGFloat(raw.y) * half)   // y up on the stick, down on screen
                ctx.fill(Path { $0.addEllipse(in: CGRect(x: pos.x - 4, y: pos.y - 4, width: 8, height: 8)) }, with: .color(.primary))
                if r > deadzone && deadzone < 1 {   // past the zone: the rescaled deflection the game receives
                    let k = (r - deadzone) / (1 - deadzone) / r
                    let out = CGPoint(x: c.x + CGFloat(raw.x * k) * half, y: c.y - CGFloat(raw.y * k) * half)
                    ctx.stroke(Path { p in p.move(to: pos); p.addLine(to: out) }, with: .color(.green), lineWidth: 1.5)
                    ctx.fill(Path { $0.addEllipse(in: CGRect(x: out.x - 5, y: out.y - 5, width: 10, height: 10)) }, with: .color(.green))
                }
            }
            .frame(width: 110, height: 110)
        }
    }
}
