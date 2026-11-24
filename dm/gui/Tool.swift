import CryptoKit
import Foundation

struct ToolFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

final class Log: @unchecked Sendable {
    static let shared = Log()
    let url: URL
    private let queue = DispatchQueue(label: "org.nativenavi.NativeNavi.log")
    private let handle: FileHandle?

    private init() {
        let fm = FileManager.default
        let dir = fm.homeDirectoryForCurrentUser.appending(path: "Library/Logs/NativeNavi", directoryHint: .isDirectory)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appending(path: "NativeNavi.log")
        let prev = dir.appending(path: "NativeNavi.prev.log")
        try? fm.removeItem(at: prev)
        try? fm.moveItem(at: url, to: prev)
        fm.createFile(atPath: url.path, contents: nil)
        handle = try? FileHandle(forWritingTo: url)
    }

    func write(_ text: String) {
        let stamp = Date.now.ISO8601Format()
        queue.async { [handle] in try? handle?.write(contentsOf: Data("\(stamp) \(text)\n".utf8)) }
    }
}

enum Output: Sendable { case out, err }

private final class Running: @unchecked Sendable {
    let process: Process
    init(_ process: Process) { self.process = process }
    func stop() { if process.isRunning { process.terminate() } }
}

/// Cancelling the calling task sends the tool SIGTERM, and the call still waits for it to end
func runTool(_ exe: URL, _ args: [String], env: [String: String], cwd: URL? = nil,
             line: @escaping @MainActor @Sendable (Output, String) -> Void = { _, _ in }) async throws -> Int32 {
    let p = Process()
    p.executableURL = exe
    p.arguments = args
    p.environment = env
    if let cwd { p.currentDirectoryURL = cwd }
    let out = Pipe(), err = Pipe()
    p.standardOutput = out
    p.standardError = err
    p.standardInput = FileHandle.nullDevice
    let exit = AsyncStream<Int32> { c in
        p.terminationHandler = { c.yield($0.terminationStatus); c.finish() }
    }
    Log.shared.write("$ " + ([exe.path] + args).map { $0.contains(" ") ? "'\($0)'" : $0 }.joined(separator: " "))
    try p.run()
    let running = Running(p)
    // own tasks: a cancel must not stop reading the stopping tool's last lines and exit status
    let readers = [Task { await pump(out.fileHandleForReading, .out, line) }, Task { await pump(err.fileHandleForReading, .err, line) }]
    let ended = Task { () -> Int32 in
        for await s in exit { return s }
        return -1
    }
    await withTaskCancellationHandler {
        for r in readers { await r.value }
    } onCancel: {
        running.stop()
    }
    let status = await ended.value
    Log.shared.write("exit status \(status)")
    return status
}

/// Not FileHandle.bytes, where a quiet stderr stalls stdout (docs/reference/packing.md#nativenavi-stays-on-installing-the-rust-compiler-while-the-game-builds)
private func chunks(_ handle: FileHandle) -> AsyncStream<[UInt8]> {
    AsyncStream { c in
        handle.readabilityHandler = { h in
            var buf = [UInt8](repeating: 0, count: 1 << 16)
            let n = read(h.fileDescriptor, &buf, buf.count)
            if n > 0 {
                c.yield(Array(buf[..<n]))
            } else if n == 0 || (errno != EINTR && errno != EAGAIN) {
                h.readabilityHandler = nil
                c.finish()
            }
        }
    }
}

private func pump(_ handle: FileHandle, _ kind: Output, _ line: @escaping @MainActor @Sendable (Output, String) -> Void) async {
    var buffer = [UInt8]()
    for await chunk in chunks(handle) {
        for b in chunk {
            if b == 0x0A || b == 0x0D {   // rustup and xcodebuild redraw with \r
                if !buffer.isEmpty {
                    let text = String(decoding: buffer, as: UTF8.self)
                    buffer.removeAll(keepingCapacity: true)
                    if kind == .err || !text.hasPrefix("{") { Log.shared.write(text) }
                    await line(kind, text)
                }
            } else {
                buffer.append(b)
            }
        }
    }
    if !buffer.isEmpty {
        let text = String(decoding: buffer, as: UTF8.self)
        Log.shared.write(text)
        await line(kind, text)
    }
}

func capture(_ exe: String, _ args: [String], env: [String: String]? = nil) async -> String? {
    let lines = LineBox()
    guard let status = try? await runTool(URL(filePath: exe), args, env: env ?? ProcessInfo.processInfo.environment, line: { kind, text in
        if kind == .out { lines.add(text) }
    }), status == 0 else { return nil }
    return lines.text
}

private final class LineBox: @unchecked Sendable {   // written on the main actor only
    private var lines: [String] = []
    func add(_ s: String) { lines.append(s) }
    var text: String { lines.joined(separator: "\n") }
}

final class Download: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let dest: URL
    private let progress: @Sendable (Int64, Int64) -> Void
    private var continuation: CheckedContinuation<Void, Error>?
    private var result: Result<Void, Error>?
    private let lock = NSLock()

    private init(dest: URL, progress: @escaping @Sendable (Int64, Int64) -> Void) {
        self.dest = dest
        self.progress = progress
    }

    static func fetch(_ url: URL, to dest: URL, progress: @escaping @Sendable (Int64, Int64) -> Void) async throws {
        let d = Download(dest: dest, progress: progress)
        let session = URLSession(configuration: .ephemeral, delegate: d, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        Log.shared.write("download \(url.absoluteString)")
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                d.lock.withLock { d.continuation = c }
                if Task.isCancelled { d.finish(.failure(CancellationError())); return }
                session.downloadTask(with: url).resume()
            }
        } onCancel: {
            session.invalidateAndCancel()
        }
    }

    private func finish(_ r: Result<Void, Error>) {
        let c: CheckedContinuation<Void, Error>? = lock.withLock { defer { continuation = nil }; return continuation }
        c?.resume(with: r)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        progress(totalBytesWritten, totalBytesExpectedToWrite)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            result = .failure(ToolFailure("The download from \(downloadTask.originalRequest?.url?.host() ?? "the server") failed (HTTP \(http.statusCode))."))
            return
        }
        do {   // the file is gone once this returns
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: location, to: dest)
            result = .success(())
        } catch { result = .failure(error) }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { Log.shared.write("download failed: \(error.localizedDescription)") }
        finish(error.map { .failure(($0 as? URLError)?.code == .cancelled ? CancellationError() : $0) } ?? result ?? .failure(ToolFailure("The download failed.")))
    }
}

func sha256(of url: URL) throws -> String {
    let h = try FileHandle(forReadingFrom: url)
    defer { try? h.close() }
    var hasher = SHA256()
    while let data = try h.read(upToCount: 1 << 20), !data.isEmpty { hasher.update(data: data) }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}
