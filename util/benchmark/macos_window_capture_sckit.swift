import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import ApplicationServices
import AppKit
import Darwin

struct Region: Codable {
    let x: Double
    let y: Double
    let width: Double
    let height: Double

    var cgRect: CGRect {
        CGRect(x: x, y: y, width: width, height: height)
    }

    func toJSON() -> [String: Double] {
        [
            "x": x,
            "y": y,
            "width": width,
            "height": height,
        ]
    }
}

struct Calibration: Codable {
    let app: String?
    let bundleIdentifier: String?
    let windowOwnerName: String?
    let windowNameSubstring: String?
    let regionCoordinateSpace: String?
    let captureRegion: Region?
    let analysisRegionNorm: Region?

    enum CodingKeys: String, CodingKey {
        case app
        case bundleIdentifier = "bundle_identifier"
        case windowOwnerName = "window_owner_name"
        case windowNameSubstring = "window_name_substring"
        case regionCoordinateSpace = "region_coordinate_space"
        case captureRegion = "capture_region"
        case analysisRegionNorm = "analysis_region_norm"
    }
}

struct Options {
    var listWindows = false
    var calibrationPath: String?
    var eventsPath: String?
    var outputPath: String?
    var framesOutputPath: String?
    var analysisFramesDirPath: String?
    var sampleHz = 60.0
    var pixelThreshold = 0.0
    var changedFractionThreshold = 0.0
    var stableFrames = 5
    var skipFrameDiffAnalysis = false
    var timeoutSeconds = 300.0
    var queueDepth = 8
    var captureTarget = "window"
    var windowOwnerName: String?
    var windowTitleSubstring: String?
}

struct CGWindowCandidate {
    let windowID: CGWindowID
    let ownerName: String
    let title: String
    let bounds: CGRect
}

struct ResolvedWindow {
    let contentFilter: SCContentFilter
    let matchedWindowID: CGWindowID
    let bounds: CGRect
    let ownerName: String
    let title: String
    let captureTarget: String
    let displayID: CGDirectDisplayID?
    let displayBounds: CGRect?
    let pointPixelScale: Double
    let configSourceRect: CGRect
    let sourceRectAbsolute: CGRect
    let cropPoints: CGRect
    let cropPixels: CGRect
    let analysisPoints: CGRect
    let analysisPixels: CGRect
    let outputWidth: Int
    let outputHeight: Int
}

struct FrameStats {
    let count: Int
    let mean: Double?
    let median: Double?
    let min: Double?
    let max: Double?
    let p95: Double?

    func toJSON() -> [String: Any?] {
        [
            "count": count,
            "mean": mean,
            "median": median,
            "min": min,
            "max": max,
            "p95": p95,
        ]
    }
}

enum CaptureError: Error, CustomStringConvertible {
    case usage(String)
    case unsupportedOS(String)
    case fileMissing(String)
    case invalidCalibration(String)
    case windowNotFound(String)
    case streamSetup(String)
    case timedOut(String)

    var description: String {
        switch self {
        case .usage(let message),
             .unsupportedOS(let message),
             .fileMissing(let message),
             .invalidCalibration(let message),
             .windowNotFound(let message),
             .streamSetup(let message),
             .timedOut(let message):
            return message
        }
    }
}

final class JsonLineWriter {
    private let handle: FileHandle

    init(path: String) throws {
        let url = URL(fileURLWithPath: path)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        FileManager.default.createFile(atPath: url.path, contents: nil)
        self.handle = try FileHandle(forWritingTo: url)
    }

    func write(_ record: [String: Any]) throws {
        let data = try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys])
        try handle.write(contentsOf: data)
        try handle.write(contentsOf: Data([0x0a]))
    }

    func close() throws {
        try handle.close()
    }
}

final class JsonWriter {
    private let path: String

    init(path: String) {
        self.path = path
    }

    func write(_ payload: [String: Any]) throws {
        let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
        let url = URL(fileURLWithPath: path)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try data.write(to: url)
        try FileHandle(forWritingTo: url).close()
        if let handle = FileHandle(forWritingAtPath: url.path) {
            try handle.seekToEnd()
            try handle.write(contentsOf: Data([0x0a]))
            try handle.close()
        }
    }
}

func parseArgs() throws -> Options {
    var options = Options()
    var index = 1
    let args = CommandLine.arguments

    func requireValue(_ flag: String) throws -> String {
        let nextIndex = index + 1
        guard nextIndex < args.count else {
            throw CaptureError.usage("missing value for \(flag)")
        }
        index = nextIndex
        return args[nextIndex]
    }

    while index < args.count {
        let arg = args[index]
        switch arg {
        case "--help", "-h":
            throw CaptureError.usage("""
            usage: macos_window_capture_sckit [options]

              --list-windows
              --calibration PATH
              --events PATH
              --output PATH
              --frames-output PATH
              --analysis-frames-dir PATH
              --sample-hz N
              --pixel-threshold N
              --changed-fraction-threshold N
              --stable-frames N
              --skip-frame-diff-analysis
              --timeout-seconds N
              --queue-depth N
              --capture-target window|display
              --window-owner-name NAME
              --window-title-substring TEXT
            """)
        case "--list-windows":
            options.listWindows = true
        case "--calibration":
            options.calibrationPath = try requireValue(arg)
        case "--events":
            options.eventsPath = try requireValue(arg)
        case "--output":
            options.outputPath = try requireValue(arg)
        case "--frames-output":
            options.framesOutputPath = try requireValue(arg)
        case "--analysis-frames-dir":
            options.analysisFramesDirPath = try requireValue(arg)
        case "--sample-hz":
            options.sampleHz = Double(try requireValue(arg)) ?? options.sampleHz
        case "--pixel-threshold":
            options.pixelThreshold = Double(try requireValue(arg)) ?? options.pixelThreshold
        case "--changed-fraction-threshold":
            options.changedFractionThreshold = max(
                0.0,
                min(1.0, Double(try requireValue(arg)) ?? options.changedFractionThreshold)
            )
        case "--stable-frames":
            options.stableFrames = max(1, Int(try requireValue(arg)) ?? options.stableFrames)
        case "--skip-frame-diff-analysis":
            options.skipFrameDiffAnalysis = true
        case "--timeout-seconds":
            options.timeoutSeconds = max(1.0, Double(try requireValue(arg)) ?? options.timeoutSeconds)
        case "--queue-depth":
            options.queueDepth = max(1, min(8, Int(try requireValue(arg)) ?? options.queueDepth))
        case "--capture-target":
            let value = try requireValue(arg).trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard value == "window" || value == "display" else {
                throw CaptureError.usage("--capture-target must be 'window' or 'display'")
            }
            options.captureTarget = value
        case "--window-owner-name":
            options.windowOwnerName = try requireValue(arg)
        case "--window-title-substring":
            options.windowTitleSubstring = try requireValue(arg)
        default:
            throw CaptureError.usage("unrecognized argument \(arg)")
        }
        index += 1
    }

    if !options.listWindows {
        guard options.eventsPath != nil else {
            throw CaptureError.usage("--events is required unless --list-windows is used")
        }
        guard options.outputPath != nil else {
            throw CaptureError.usage("--output is required unless --list-windows is used")
        }
        if options.calibrationPath == nil
            && (options.windowOwnerName == nil || options.windowTitleSubstring == nil) {
            throw CaptureError.usage(
                "provide --calibration or both --window-owner-name and --window-title-substring"
            )
        }
    }

    return options
}

func loadCalibration(path: String) throws -> Calibration {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: url.path) else {
        throw CaptureError.fileMissing("calibration file not found: \(path)")
    }
    let data = try Data(contentsOf: url)
    return try JSONDecoder().decode(Calibration.self, from: data)
}

func listCGWindows() -> [CGWindowCandidate] {
    guard let raw = CGWindowListCopyWindowInfo(
        [.optionOnScreenOnly, .excludeDesktopElements],
        kCGNullWindowID
    ) as? [[String: Any]] else {
        return []
    }

    return raw.compactMap { entry in
        guard let windowID = entry[kCGWindowNumber as String] as? UInt32,
              let ownerName = entry[kCGWindowOwnerName as String] as? String,
              let boundsDict = entry[kCGWindowBounds as String] as? [String: Any],
              let bounds = CGRect(dictionaryRepresentation: boundsDict as CFDictionary),
              bounds.width > 0,
              bounds.height > 0 else {
            return nil
        }
        let title = (entry[kCGWindowName as String] as? String) ?? ""
        return CGWindowCandidate(
            windowID: CGWindowID(windowID),
            ownerName: ownerName,
            title: title,
            bounds: bounds
        )
    }
}

func hostTimeToNanoseconds(_ hostTime: UInt64) -> UInt64 {
    var timebase = mach_timebase_info_data_t()
    mach_timebase_info(&timebase)
    return UInt64((Double(hostTime) * Double(timebase.numer)) / Double(timebase.denom))
}

func frameStatusName(_ status: SCFrameStatus) -> String {
    switch status {
    case .complete: return "complete"
    case .idle: return "idle"
    case .blank: return "blank"
    case .suspended: return "suspended"
    case .started: return "started"
    case .stopped: return "stopped"
    @unknown default: return "unknown"
    }
}

func stats(_ values: [Double]) -> [String: Any?] {
    guard !values.isEmpty else {
        return [
            "count": 0,
            "mean": nil,
            "median": nil,
            "min": nil,
            "max": nil,
            "p95": nil,
        ]
    }

    let ordered = values.sorted()
    let count = ordered.count
    func percentile(_ q: Double) -> Double {
        if count == 1 { return ordered[0] }
        let pos = q * Double(count - 1)
        let lower = Int(floor(pos))
        let upper = min(lower + 1, count - 1)
        if lower == upper { return ordered[lower] }
        let frac = pos - Double(lower)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * frac
    }

    return [
        "count": count,
        "mean": ordered.reduce(0.0, +) / Double(count),
        "median": percentile(0.5),
        "min": ordered.first,
        "max": ordered.last,
        "p95": percentile(0.95),
    ]
}

func normalizedSubregion(_ normalized: CGRect, within rect: CGRect) -> CGRect {
    CGRect(
        x: rect.origin.x + rect.width * normalized.origin.x,
        y: rect.origin.y + rect.height * normalized.origin.y,
        width: rect.width * normalized.width,
        height: rect.height * normalized.height
    )
}

func copyAnalysisFrameBGRA32(
    pixelBuffer: CVPixelBuffer,
    analysisRect: CGRect
) -> [UInt32]? {
    let integralRect = analysisRect.integral
    guard integralRect.width > 0.0, integralRect.height > 0.0 else {
        return nil
    }

    CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else {
        return nil
    }

    let bufferWidth = CVPixelBufferGetWidth(pixelBuffer)
    let bufferHeight = CVPixelBufferGetHeight(pixelBuffer)
    let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let bytesPerPixel = 4

    let clampedRect = integralRect.intersection(
        CGRect(x: 0, y: 0, width: bufferWidth, height: bufferHeight)
    )
    guard clampedRect.width > 0.0, clampedRect.height > 0.0 else {
        return nil
    }

    let sourceWidth = Int(clampedRect.width)
    let sourceHeight = Int(clampedRect.height)
    let startX = Int(clampedRect.origin.x)
    let startY = Int(clampedRect.origin.y)
    let copyBytesPerRow = sourceWidth * bytesPerPixel
    let rawPointer = baseAddress.assumingMemoryBound(to: UInt8.self)
    var result = [UInt32](repeating: 0, count: sourceWidth * sourceHeight)
    result.withUnsafeMutableBytes { destination in
        guard let destBase = destination.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
            return
        }
        for row in 0..<sourceHeight {
            let src = rawPointer.advanced(by: (startY + row) * bytesPerRow + startX * bytesPerPixel)
            let dst = destBase.advanced(by: row * copyBytesPerRow)
            dst.update(from: src, count: copyBytesPerRow)
        }
    }

    return result
}

func writeAnalysisFrameRawBGRA32(
    frame: [UInt32],
    width: Int,
    height: Int,
    path: String
) throws {
    guard width > 0, height > 0, frame.count == width * height else {
        throw CaptureError.streamSetup(
            "invalid analysis frame dimensions \(width)x\(height) for \(frame.count) pixels"
        )
    }

    let url = URL(fileURLWithPath: path)
    try FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(),
        withIntermediateDirectories: true
    )
    let byteCount = frame.count * MemoryLayout<UInt32>.size
    let data = frame.withUnsafeBufferPointer { buffer in
        Data(buffer: UnsafeBufferPointer<UInt8>(
            start: UnsafeRawPointer(buffer.baseAddress!).assumingMemoryBound(to: UInt8.self),
            count: byteCount
        ))
    }
    try data.write(to: url, options: .atomic)
}

func writeAnalysisFramePNG(
    frame: [UInt32],
    width: Int,
    height: Int,
    path: String
) throws {
    guard width > 0, height > 0, frame.count == width * height else {
        throw CaptureError.streamSetup(
            "invalid analysis frame dimensions \(width)x\(height) for \(frame.count) pixels"
        )
    }

    let url = URL(fileURLWithPath: path)
    try FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(),
        withIntermediateDirectories: true
    )
    let byteCount = frame.count * MemoryLayout<UInt32>.size
    let data = frame.withUnsafeBufferPointer { buffer in
        Data(buffer: UnsafeBufferPointer<UInt8>(
            start: UnsafeRawPointer(buffer.baseAddress!).assumingMemoryBound(to: UInt8.self),
            count: byteCount
        ))
    }
    let provider = CGDataProvider(data: data as CFData)
    let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue)
        .union(.byteOrder32Little)
    guard let provider,
          let image = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
          )
    else {
        throw CaptureError.streamSetup("failed to create analysis PNG image")
    }
    let bitmap = NSBitmapImageRep(cgImage: image)
    guard let pngData = bitmap.representation(using: NSBitmapImageRep.FileType.png, properties: [:]) else {
        throw CaptureError.streamSetup("failed to encode analysis PNG")
    }
    try pngData.write(to: url, options: Data.WritingOptions.atomic)
}

func diffMetrics(
    current: [UInt32],
    reference: [UInt32]
) -> (meanAbsNormalized: Double?, changedFraction: Double) {
    guard !current.isEmpty, current.count == reference.count else {
        return (nil, 0.0)
    }

    var changedCount = 0
    for (lhs, rhs) in zip(current, reference) {
        if lhs != rhs {
            changedCount += 1
        }
    }

    let pixelCount = Double(current.count)
    return (
        meanAbsNormalized: nil,
        changedFraction: Double(changedCount) / pixelCount
    )
}

final class ActionTracker {
    let name: String
    let actionStartWallNs: UInt64
    let actionStartMonotonicNs: UInt64
    var dragStartWallNs: UInt64?
    var dragStartMonotonicNs: UInt64?
    var dragEndWallNs: UInt64?
    var dragEndMonotonicNs: UInt64?
    var actionEndWallNs: UInt64?
    var actionEndMonotonicNs: UInt64?
    var baselineAnalysisFrame: [UInt32]?
    var baselineFrameMonotonicNs: UInt64?
    var firstVisibleMonotonicNs: UInt64?
    var stableMonotonicNs: UInt64?
    var stableStreak = 0
    var stableStartMonotonicNs: UInt64?

    init(
        name: String,
        actionStartWallNs: UInt64,
        actionStartMonotonicNs: UInt64
    ) {
        self.name = name
        self.actionStartWallNs = actionStartWallNs
        self.actionStartMonotonicNs = actionStartMonotonicNs
    }

    var startWallNs: UInt64 {
        dragStartWallNs ?? actionStartWallNs
    }

    var startMonotonicNs: UInt64 {
        dragStartMonotonicNs ?? actionStartMonotonicNs
    }

    var endWallNs: UInt64? {
        dragEndWallNs ?? actionEndWallNs
    }

    var endMonotonicNs: UInt64? {
        dragEndMonotonicNs ?? actionEndMonotonicNs
    }
}

struct CapturedFrame {
    let frameIndex: Int
    let status: SCFrameStatus
    let statusRaw: Int
    let wallTimeNs: UInt64
    let monotonicNs: UInt64
    let displayMonotonicNs: UInt64
    let analysisWidth: Int
    let analysisHeight: Int
    let analysisSampleCount: Int
    let analysisFrame: [UInt32]
}

final class CaptureController: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    private let resolvedWindow: ResolvedWindow
    private let eventsPath: String
    private let outputPath: String
    private let framesOutputPath: String
    private let analysisFramesDirPath: String?
    private let sampleHz: Double
    private let pixelThreshold: Double
    private let changedFractionThreshold: Double
    private let stableFrames: Int
    private let skipFrameDiffAnalysis: Bool
    private let timeoutSeconds: Double
    private let queueDepth: Int

    private let processingQueue = DispatchQueue(
        label: "atlas.sckit.capture",
        qos: .userInteractive
    )
    private let controlQueue = DispatchQueue(
        label: "atlas.sckit.capture.control",
        qos: .userInitiated
    )
    private let framesWriter: JsonLineWriter
    private let summaryWriter: JsonWriter

    private var stream: SCStream?
    private var eventTimer: DispatchSourceTimer?
    private var timeoutTimer: DispatchSourceTimer?
    private var processedEventCount = 0

    private var writtenFrameTimestampsNs: [UInt64] = []
    private var capturedFrames: [CapturedFrame] = []
    private var frameCount = 0
    private var appName: String?
    private var sessionEnded = false
    private var currentAction: ActionTracker?
    private var completedActions: [ActionTracker] = []
    private var stopRequested = false
    private var finishContinuation: CheckedContinuation<Void, Error>?

    init(
        resolvedWindow: ResolvedWindow,
        eventsPath: String,
        outputPath: String,
        framesOutputPath: String,
        analysisFramesDirPath: String?,
        sampleHz: Double,
        pixelThreshold: Double,
        changedFractionThreshold: Double,
        stableFrames: Int,
        skipFrameDiffAnalysis: Bool,
        timeoutSeconds: Double,
        queueDepth: Int
    ) throws {
        self.resolvedWindow = resolvedWindow
        self.eventsPath = eventsPath
        self.outputPath = outputPath
        self.framesOutputPath = framesOutputPath
        self.analysisFramesDirPath = analysisFramesDirPath
        self.sampleHz = sampleHz
        self.pixelThreshold = pixelThreshold
        self.changedFractionThreshold = changedFractionThreshold
        self.stableFrames = stableFrames
        self.skipFrameDiffAnalysis = skipFrameDiffAnalysis
        self.timeoutSeconds = timeoutSeconds
        self.queueDepth = queueDepth
        self.framesWriter = try JsonLineWriter(path: framesOutputPath)
        self.summaryWriter = JsonWriter(path: outputPath)
        if let analysisFramesDirPath {
            try FileManager.default.createDirectory(
                at: URL(fileURLWithPath: analysisFramesDirPath),
                withIntermediateDirectories: true
            )
        }
    }

    func run() async throws {
        try await withCheckedThrowingContinuation { continuation in
            self.finishContinuation = continuation
            processingQueue.async {
                do {
                    try self.startLocked()
                } catch {
                    self.failLocked(error)
                }
            }
        }
    }

    private func startLocked() throws {
        let config = SCStreamConfiguration()
        config.width = resolvedWindow.outputWidth
        config.height = resolvedWindow.outputHeight
        if sampleHz > 0.0 {
            config.minimumFrameInterval = CMTime(seconds: 1.0 / sampleHz, preferredTimescale: 600)
        } else {
            config.minimumFrameInterval = .zero
        }
        config.pixelFormat = kCVPixelFormatType_32BGRA
        config.showsCursor = false
        config.queueDepth = queueDepth
        config.scalesToFit = false
        if resolvedWindow.captureTarget == "window" {
            config.ignoreShadowsSingleWindow = true
        }
        if #available(macOS 14.0, *) {
            config.captureResolution = .nominal
        }
        config.sourceRect = resolvedWindow.configSourceRect

        let stream = SCStream(
            filter: resolvedWindow.contentFilter,
            configuration: config,
            delegate: self
        )
        self.stream = stream

        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: processingQueue)
        } catch {
            throw CaptureError.streamSetup("failed to add ScreenCaptureKit output: \(error.localizedDescription)")
        }

        let eventTimer = DispatchSource.makeTimerSource(queue: controlQueue)
        eventTimer.schedule(deadline: .now(), repeating: .milliseconds(10))
        eventTimer.setEventHandler { [weak self] in
            self?.processingQueue.async {
                self?.pollEventsLocked()
            }
        }
        self.eventTimer = eventTimer
        eventTimer.resume()

        let timeoutTimer = DispatchSource.makeTimerSource(queue: controlQueue)
        timeoutTimer.schedule(deadline: .now() + timeoutSeconds)
        timeoutTimer.setEventHandler { [weak self] in
            self?.processingQueue.async {
                self?.stopLocked()
            }
        }
        self.timeoutTimer = timeoutTimer
        timeoutTimer.resume()

        stream.startCapture { [weak self] error in
            self?.processingQueue.async {
                if let error {
                    self?.failLocked(error)
                }
            }
        }
    }

    private func pollEventsLocked() {
        guard FileManager.default.fileExists(atPath: eventsPath) else {
            return
        }

        do {
            let text = try String(contentsOfFile: eventsPath, encoding: .utf8)
            let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
            guard processedEventCount < lines.count else {
                return
            }
            for line in lines[processedEventCount...] {
                guard let data = String(line).data(using: .utf8),
                      let record = try JSONSerialization.jsonObject(with: data) as? [String: Any]
                else {
                    continue
                }
                applyEventLocked(record)
            }
            processedEventCount = lines.count
        } catch {
            failLocked(error)
        }
    }

    private func applyEventLocked(_ record: [String: Any]) {
        guard let name = record["event"] as? String else {
            return
        }
        switch name {
        case "session_start":
            if let app = record["app"] as? String {
                appName = app
            }
        case "action_start":
            guard let actionName = record["action"] as? String else {
                return
            }
            currentAction = ActionTracker(
                name: actionName,
                actionStartWallNs: uint64(record["wall_time_ns"]),
                actionStartMonotonicNs: uint64(record["monotonic_ns"])
            )
        case "drag_start":
            guard let actionName = record["action"] as? String,
                  let action = currentAction,
                  action.name == actionName else {
                return
            }
            action.dragStartWallNs = uint64(record["wall_time_ns"])
            action.dragStartMonotonicNs = uint64(record["monotonic_ns"])
        case "drag_end":
            guard let actionName = record["action"] as? String,
                  let action = currentAction,
                  action.name == actionName else {
                return
            }
            action.dragEndWallNs = uint64(record["wall_time_ns"])
            action.dragEndMonotonicNs = uint64(record["monotonic_ns"])
        case "action_end":
            guard let actionName = record["action"] as? String else {
                return
            }
            if let action = currentAction, action.name == actionName {
                action.actionEndWallNs = uint64(record["wall_time_ns"])
                action.actionEndMonotonicNs = uint64(record["monotonic_ns"])
                completedActions.append(action)
                currentAction = nil
                requestStopIfNeededLocked()
                return
            }
        case "session_end":
            sessionEnded = true
            requestStopIfNeededLocked()
        default:
            break
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of outputType: SCStreamOutputType) {
        guard outputType == .screen else {
            return
        }

        let callbackWallNs = UInt64(Date().timeIntervalSince1970 * 1_000_000_000.0)
        let callbackMonotonicNs = DispatchTime.now().uptimeNanoseconds
        guard let attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let attachments = attachmentsArray.first,
              let statusRaw = attachments[.status] as? Int,
              let status = SCFrameStatus(rawValue: statusRaw) else {
            return
        }

        let displayHostTime = (attachments[.displayTime] as? UInt64) ?? 0
        let displayMonotonicNs = displayHostTime > 0 ? hostTimeToNanoseconds(displayHostTime) : callbackMonotonicNs
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return
        }
        let analysisRect = resolvedWindow.analysisPixels.integral
        guard let analysisFrame = copyAnalysisFrameBGRA32(
            pixelBuffer: pixelBuffer,
            analysisRect: analysisRect
        ) else {
            return
        }

        capturedFrames.append(
            CapturedFrame(
                frameIndex: frameCount,
                status: status,
                statusRaw: status.rawValue,
                wallTimeNs: callbackWallNs,
                monotonicNs: callbackMonotonicNs,
                displayMonotonicNs: displayMonotonicNs,
                analysisWidth: Int(analysisRect.width),
                analysisHeight: Int(analysisRect.height),
                analysisSampleCount: analysisFrame.count,
                analysisFrame: analysisFrame
            )
        )
        frameCount += 1
        writtenFrameTimestampsNs.append(displayMonotonicNs)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        processingQueue.async {
            self.failLocked(error)
        }
    }

    private func requestStopIfNeededLocked() {
        if sessionEnded {
            stopLocked()
        }
    }

    private func stopLocked() {
        guard !stopRequested else {
            return
        }
        stopRequested = true
        eventTimer?.cancel()
        timeoutTimer?.cancel()
        eventTimer = nil
        timeoutTimer = nil

        stream?.stopCapture(completionHandler: nil)
        finishLocked()
    }

    private func finishLocked() {
        do {
            let frameRecords = analyzeFramesLocked()
            for record in frameRecords {
                try framesWriter.write(record)
            }
            try framesWriter.close()
            try summaryWriter.write(summaryPayload())
            finishContinuation?.resume()
            finishContinuation = nil
        } catch {
            failLocked(error)
        }
    }

    private func failLocked(_ error: Error) {
        if stopRequested, finishContinuation == nil {
            return
        }
        stopRequested = true
        eventTimer?.cancel()
        timeoutTimer?.cancel()
        eventTimer = nil
        timeoutTimer = nil
        stream?.stopCapture(completionHandler: nil)
        try? framesWriter.close()
        finishContinuation?.resume(throwing: error)
        finishContinuation = nil
    }

    private func actionOwningFrame(_ frameTimeNs: UInt64, actions: [ActionTracker]) -> ActionTracker? {
        guard !actions.isEmpty else {
            return nil
        }
        for (index, action) in actions.enumerated() {
            let nextStartNs = index + 1 < actions.count ? actions[index + 1].actionStartMonotonicNs : nil
            if frameTimeNs < action.actionStartMonotonicNs {
                continue
            }
            if let nextStartNs, frameTimeNs >= nextStartNs {
                continue
            }
            return action
        }
        return nil
    }

    private func analyzeFramesLocked() -> [[String: Any]] {
        let actions = completedActions.sorted { $0.actionStartMonotonicNs < $1.actionStartMonotonicNs }
        let useAnyPixelChange = pixelThreshold <= 0.0 && changedFractionThreshold <= 0.0

        if !skipFrameDiffAnalysis {
            for action in actions {
                let baselineAnchorNs = action.dragStartMonotonicNs ?? action.startMonotonicNs
                if let baselineFrame = capturedFrames.last(where: { $0.displayMonotonicNs < baselineAnchorNs }) {
                    action.baselineAnalysisFrame = baselineFrame.analysisFrame
                    action.baselineFrameMonotonicNs = baselineFrame.displayMonotonicNs
                } else if let firstFrame = capturedFrames.first(where: { $0.displayMonotonicNs >= baselineAnchorNs }) {
                    action.baselineAnalysisFrame = firstFrame.analysisFrame
                    action.baselineFrameMonotonicNs = firstFrame.displayMonotonicNs
                }
                action.firstVisibleMonotonicNs = nil
                action.stableMonotonicNs = nil
                action.stableStartMonotonicNs = nil
                action.stableStreak = 0
            }
        }

        var previousFrame: CapturedFrame?
        let analysisFrameDigits = max(4, String(max(0, capturedFrames.count - 1)).count)
        var frameRecords: [[String: Any]] = []
        frameRecords.reserveCapacity(capturedFrames.count)

        for frame in capturedFrames {
            let owningAction = actionOwningFrame(frame.displayMonotonicNs, actions: actions)
            let prevMetrics = skipFrameDiffAnalysis
                ? nil
                : previousFrame.flatMap {
                    diffMetrics(current: frame.analysisFrame, reference: $0.analysisFrame)
                }
            let diffPrev = prevMetrics?.changedFraction
            let meanAbsDiffPrev = prevMetrics?.meanAbsNormalized
            let significantPrevChange = skipFrameDiffAnalysis
                ? false
                : (
                    useAnyPixelChange
                    ? (prevMetrics?.changedFraction ?? 0.0) > 0.0
                    : (prevMetrics?.changedFraction ?? 0.0) >= changedFractionThreshold
                )

            var diffBaseline: Double?
            var meanAbsDiffBaseline: Double?
            var significantBaselineChange = false
            var firstVisibleTriggered = false
            var stableTriggered = false
            let activeActionName = owningAction?.name
            let activeActionEndMonotonicNs = owningAction?.endMonotonicNs

            if !skipFrameDiffAnalysis, let action = owningAction {
                let visibleAnchorNs = action.dragStartMonotonicNs ?? action.startMonotonicNs
                if let baselineFrame = action.baselineAnalysisFrame, frame.displayMonotonicNs >= visibleAnchorNs {
                    let baselineMetrics = diffMetrics(
                        current: frame.analysisFrame,
                        reference: baselineFrame
                    )
                    diffBaseline = baselineMetrics.changedFraction
                    meanAbsDiffBaseline = baselineMetrics.meanAbsNormalized
                    significantBaselineChange = useAnyPixelChange
                        ? baselineMetrics.changedFraction > 0.0
                        : baselineMetrics.changedFraction >= changedFractionThreshold
                    if action.firstVisibleMonotonicNs == nil, significantBaselineChange {
                        action.firstVisibleMonotonicNs = frame.displayMonotonicNs
                        firstVisibleTriggered = true
                    }
                }

                if let endNs = action.endMonotonicNs, frame.displayMonotonicNs >= endNs {
                    if !significantPrevChange {
                        if action.stableStreak == 0 {
                            action.stableStartMonotonicNs = frame.displayMonotonicNs
                        }
                        action.stableStreak += 1
                        if action.stableMonotonicNs == nil, action.stableStreak >= stableFrames {
                            action.stableMonotonicNs = action.stableStartMonotonicNs
                            stableTriggered = true
                        }
                    } else {
                        action.stableStreak = 0
                        action.stableStartMonotonicNs = nil
                    }
                }
            }

            var frameRecord: [String: Any] = [
                "frame_index": frame.frameIndex,
                "status": frameStatusName(frame.status),
                "status_raw": frame.statusRaw,
                "wall_time_ns": frame.wallTimeNs,
                "monotonic_ns": frame.monotonicNs,
                "display_monotonic_ns": frame.displayMonotonicNs,
                "active_action": activeActionName as Any,
                "active_action_end_monotonic_ns": activeActionEndMonotonicNs as Any,
                "diff_prev": diffPrev as Any,
                "diff_action_baseline": diffBaseline as Any,
                "mean_abs_diff_prev": meanAbsDiffPrev as Any,
                "mean_abs_diff_action_baseline": meanAbsDiffBaseline as Any,
                "significant_change_prev": significantPrevChange,
                "significant_change_action_baseline": significantBaselineChange,
                "first_visible_triggered": firstVisibleTriggered,
                "stable_triggered": stableTriggered,
                "stable_frames_required": stableFrames,
                "pixel_threshold": pixelThreshold,
                "changed_fraction_threshold": changedFractionThreshold,
                "analysis_width": frame.analysisWidth,
                "analysis_height": frame.analysisHeight,
                "analysis_sample_count": frame.analysisSampleCount,
            ]
            if let analysisFramesDirPath,
               owningAction != nil,
               (skipFrameDiffAnalysis || significantBaselineChange) {
                let pngPath = URL(fileURLWithPath: analysisFramesDirPath)
                    .appendingPathComponent(
                        String(
                            format: "frame_%0\(analysisFrameDigits)d.png",
                            frame.frameIndex
                        )
                    )
                    .path
                do {
                    try writeAnalysisFramePNG(
                        frame: frame.analysisFrame,
                        width: frame.analysisWidth,
                        height: frame.analysisHeight,
                        path: pngPath
                    )
                    frameRecord["analysis_frame_png"] = pngPath
                } catch {
                    failLocked(error)
                    return frameRecords
                }
            }
            frameRecords.append(frameRecord)
            previousFrame = frame
        }

        return frameRecords
    }

    private func summaryPayload() -> [String: Any] {
        func actionEntry(_ action: ActionTracker, completed: Bool) -> [String: Any] {
            let anchorStartMonotonicNs = action.startMonotonicNs
            let anchorStartWallNs = action.startWallNs
            let anchorEndMonotonicNs = action.endMonotonicNs
            let anchorEndWallNs = action.endWallNs
            var entry: [String: Any] = [
                "action": action.name,
                "completed": completed,
                "anchor_start": action.dragStartMonotonicNs != nil ? "drag_start" : "action_start",
                "anchor_end": action.dragEndMonotonicNs != nil ? "drag_end" : "action_end",
                "action_start_wall_ns": action.actionStartWallNs,
                "action_start_monotonic_ns": action.actionStartMonotonicNs,
                "drag_start_wall_ns": action.dragStartWallNs as Any,
                "drag_start_monotonic_ns": action.dragStartMonotonicNs as Any,
                "drag_end_wall_ns": action.dragEndWallNs as Any,
                "drag_end_monotonic_ns": action.dragEndMonotonicNs as Any,
                "action_end_wall_ns": action.actionEndWallNs as Any,
                "action_end_monotonic_ns": action.actionEndMonotonicNs as Any,
                "start_wall_ns": anchorStartWallNs,
                "start_monotonic_ns": anchorStartMonotonicNs,
                "end_wall_ns": anchorEndWallNs as Any,
                "end_monotonic_ns": anchorEndMonotonicNs as Any,
                "baseline_frame_monotonic_ns": action.baselineFrameMonotonicNs as Any,
                "first_visible_monotonic_ns": action.firstVisibleMonotonicNs as Any,
                "stable_monotonic_ns": action.stableMonotonicNs as Any,
            ]
            if let firstVisible = action.firstVisibleMonotonicNs {
                entry["first_visible_ms_from_start"] = Double(firstVisible - anchorStartMonotonicNs) / 1e6
            }
            if let end = anchorEndMonotonicNs, let stable = action.stableMonotonicNs {
                entry["stable_ms_from_end"] = Double(stable - end) / 1e6
                entry["stable_ms_from_start"] = Double(stable - anchorStartMonotonicNs) / 1e6
            }
            return entry
        }

        var actions: [[String: Any]] = []
        for action in completedActions {
            actions.append(actionEntry(action, completed: true))
        }
        if let action = currentAction,
           action.dragStartMonotonicNs != nil || action.firstVisibleMonotonicNs != nil {
            actions.append(actionEntry(action, completed: false))
        }

        let writtenFrameIntervalsMs = zip(
            writtenFrameTimestampsNs, writtenFrameTimestampsNs.dropFirst()
        ).map { Double($1 - $0) / 1e6 }

        return [
            "app": appName as Any,
            "backend": "ScreenCaptureKit",
            "window": [
                "owner_name": resolvedWindow.ownerName,
                "title": resolvedWindow.title,
                "window_id": Int(resolvedWindow.matchedWindowID),
                "capture_target": resolvedWindow.captureTarget,
                "display_id": resolvedWindow.displayID.map { Int($0) } as Any,
                "point_pixel_scale": resolvedWindow.pointPixelScale,
                "bounds": [
                    "x": resolvedWindow.bounds.origin.x,
                    "y": resolvedWindow.bounds.origin.y,
                    "width": resolvedWindow.bounds.width,
                    "height": resolvedWindow.bounds.height,
                ],
            ],
            "display_bounds_points": resolvedWindow.displayBounds.map { bounds in
                [
                    "x": bounds.origin.x,
                    "y": bounds.origin.y,
                    "width": bounds.width,
                    "height": bounds.height,
                ]
            } as Any,
            "config_source_rect_points": [
                "x": resolvedWindow.configSourceRect.origin.x,
                "y": resolvedWindow.configSourceRect.origin.y,
                "width": resolvedWindow.configSourceRect.width,
                "height": resolvedWindow.configSourceRect.height,
            ],
            "capture_source_rect_absolute_points": [
                "x": resolvedWindow.sourceRectAbsolute.origin.x,
                "y": resolvedWindow.sourceRectAbsolute.origin.y,
                "width": resolvedWindow.sourceRectAbsolute.width,
                "height": resolvedWindow.sourceRectAbsolute.height,
            ],
            "capture_region_points": [
                "x": resolvedWindow.cropPoints.origin.x,
                "y": resolvedWindow.cropPoints.origin.y,
                "width": resolvedWindow.cropPoints.width,
                "height": resolvedWindow.cropPoints.height,
            ],
            "capture_region_pixels": [
                "x": resolvedWindow.cropPixels.origin.x,
                "y": resolvedWindow.cropPixels.origin.y,
                "width": resolvedWindow.cropPixels.width,
                "height": resolvedWindow.cropPixels.height,
            ],
            "analysis_region_points": [
                "x": resolvedWindow.analysisPoints.origin.x,
                "y": resolvedWindow.analysisPoints.origin.y,
                "width": resolvedWindow.analysisPoints.width,
                "height": resolvedWindow.analysisPoints.height,
            ],
            "analysis_region_pixels": [
                "x": resolvedWindow.analysisPixels.origin.x,
                "y": resolvedWindow.analysisPixels.origin.y,
                "width": resolvedWindow.analysisPixels.width,
                "height": resolvedWindow.analysisPixels.height,
            ],
            "frames_output": framesOutputPath,
            "analysis_frames_dir": analysisFramesDirPath as Any,
            "frame_count": frameCount,
            "sample_hz_requested": sampleHz,
            "pixel_threshold": pixelThreshold,
            "changed_fraction_threshold": changedFractionThreshold,
            "stable_frames": stableFrames,
            "timestamp_domain": "display_monotonic_ns",
            "observed_sample_interval_ms": stats(writtenFrameIntervalsMs),
            "observed_sample_hz": (
                writtenFrameIntervalsMs.isEmpty
                ? nil
                : (1000.0 / (writtenFrameIntervalsMs.reduce(0.0, +) / Double(writtenFrameIntervalsMs.count)))
            ) as Any,
            "actions": actions,
        ]
    }
}

func uint64(_ value: Any?) -> UInt64 {
    if let value = value as? UInt64 {
        return value
    }
    if let value = value as? Int {
        return UInt64(value)
    }
    if let value = value as? NSNumber {
        return value.uint64Value
    }
    if let value = value as? String, let parsed = UInt64(value) {
        return parsed
    }
    return 0
}

struct MatchedDisplay {
    let scDisplay: SCDisplay
    let bounds: CGRect
}

func resolvedDisplay(
    for cropAbsolute: CGRect,
    shareableContent: SCShareableContent
) -> MatchedDisplay? {
    var best: MatchedDisplay?
    var bestArea = 0.0
    for display in shareableContent.displays {
        let bounds = CGDisplayBounds(display.displayID)
        let overlap = cropAbsolute.intersection(bounds)
        let area = overlap.isNull ? 0.0 : overlap.width * overlap.height
        if area > bestArea {
            bestArea = area
            best = MatchedDisplay(scDisplay: display, bounds: bounds)
        }
    }
    return best
}

func resolveWindow(options: Options, calibration: Calibration?) async throws -> ResolvedWindow {
    let ownerMatch = (options.windowOwnerName ?? calibration?.windowOwnerName ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    let titleMatch = (options.windowTitleSubstring ?? calibration?.windowNameSubstring ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()

    let cgWindows = listCGWindows()
    guard let matchedCGWindow = cgWindows.first(where: { candidate in
        let ownerOK = ownerMatch.isEmpty || candidate.ownerName.lowercased().contains(ownerMatch)
        let titleOK = titleMatch.isEmpty || candidate.title.lowercased().contains(titleMatch)
        return ownerOK && titleOK
    }) else {
        throw CaptureError.windowNotFound("no on-screen CG window matched owner=\(ownerMatch) title=\(titleMatch)")
    }

    let shareableContent = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: true)
    guard let matchedSCWindow = shareableContent.windows.first(where: { $0.windowID == matchedCGWindow.windowID }) else {
        throw CaptureError.windowNotFound("ScreenCaptureKit could not resolve window id \(matchedCGWindow.windowID)")
    }

    let windowFilter = SCContentFilter(desktopIndependentWindow: matchedSCWindow)
    let regionCoordinateSpace = (
        calibration?.regionCoordinateSpace?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        ?? "absolute"
    )
    let captureRect: CGRect
    if let configuredRegion = calibration?.captureRegion?.cgRect {
        if regionCoordinateSpace == "window-relative" {
            captureRect = CGRect(
                x: matchedCGWindow.bounds.origin.x + configuredRegion.origin.x,
                y: matchedCGWindow.bounds.origin.y + configuredRegion.origin.y,
                width: configuredRegion.width,
                height: configuredRegion.height
            )
        } else {
            captureRect = configuredRegion
        }
    } else {
        captureRect = matchedCGWindow.bounds
    }
    let cropAbsolute = captureRect.intersection(matchedCGWindow.bounds)
    guard !cropAbsolute.isNull, cropAbsolute.width > 0.0, cropAbsolute.height > 0.0 else {
        throw CaptureError.invalidCalibration("capture region does not intersect the selected window")
    }
    let cropPoints = CGRect(
        x: cropAbsolute.origin.x - matchedCGWindow.bounds.origin.x,
        y: cropAbsolute.origin.y - matchedCGWindow.bounds.origin.y,
        width: cropAbsolute.width,
        height: cropAbsolute.height
    )

    let captureTarget = options.captureTarget
    let contentFilter: SCContentFilter
    let configSourceRect: CGRect
    let displayID: CGDirectDisplayID?
    let displayBounds: CGRect?
    if captureTarget == "display" {
        guard let matchedDisplay = resolvedDisplay(
            for: cropAbsolute,
            shareableContent: shareableContent
        ) else {
            throw CaptureError.windowNotFound(
                "could not resolve a display for capture rect \(cropAbsolute.debugDescription)"
            )
        }
        contentFilter = SCContentFilter(
            display: matchedDisplay.scDisplay,
            excludingWindows: []
        )
        configSourceRect = CGRect(
            x: cropAbsolute.origin.x - matchedDisplay.bounds.origin.x,
            y: cropAbsolute.origin.y - matchedDisplay.bounds.origin.y,
            width: cropAbsolute.width,
            height: cropAbsolute.height
        )
        displayID = matchedDisplay.scDisplay.displayID
        displayBounds = matchedDisplay.bounds
    } else {
        contentFilter = windowFilter
        // Window capture expects sourceRect in window-local points, not absolute screen points.
        configSourceRect = cropPoints
        displayID = nil
        displayBounds = nil
    }
    let pointPixelScale = Double(contentFilter.pointPixelScale)
    let scale = pointPixelScale > 0.0 ? pointPixelScale : 1.0

    let cropPixels = CGRect(
        x: 0.0,
        y: 0.0,
        width: round(cropPoints.width * scale),
        height: round(cropPoints.height * scale)
    )
    let analysisRegionNorm = calibration?.analysisRegionNorm?.cgRect ?? CGRect(x: 0, y: 0, width: 1, height: 1)
    let normalizedAnalysis = CGRect(
        x: min(max(analysisRegionNorm.origin.x, 0.0), 1.0),
        y: min(max(analysisRegionNorm.origin.y, 0.0), 1.0),
        width: min(max(analysisRegionNorm.width, 0.0), 1.0),
        height: min(max(analysisRegionNorm.height, 0.0), 1.0)
    )
    let localCapturePoints = CGRect(x: 0.0, y: 0.0, width: cropPoints.width, height: cropPoints.height)
    let analysisPoints = normalizedSubregion(normalizedAnalysis, within: localCapturePoints)
        .intersection(localCapturePoints)
    guard !analysisPoints.isNull, analysisPoints.width > 0.0, analysisPoints.height > 0.0 else {
        throw CaptureError.invalidCalibration("analysis region does not intersect the selected capture region")
    }
    let analysisPixels = CGRect(
        x: round(analysisPoints.origin.x * scale),
        y: round(analysisPoints.origin.y * scale),
        width: round(analysisPoints.width * scale),
        height: round(analysisPoints.height * scale)
    )

    return ResolvedWindow(
        contentFilter: contentFilter,
        matchedWindowID: matchedCGWindow.windowID,
        bounds: matchedCGWindow.bounds,
        ownerName: matchedCGWindow.ownerName,
        title: matchedCGWindow.title,
        captureTarget: captureTarget,
        displayID: displayID,
        displayBounds: displayBounds,
        pointPixelScale: scale,
        configSourceRect: configSourceRect,
        sourceRectAbsolute: cropAbsolute,
        cropPoints: cropPoints,
        cropPixels: cropPixels,
        analysisPoints: analysisPoints,
        analysisPixels: analysisPixels,
        outputWidth: max(1, Int(round(cropPoints.width * scale))),
        outputHeight: max(1, Int(round(cropPoints.height * scale)))
    )
}

func listWindows() async throws {
    let cgWindows = listCGWindows()
    let shareableContent = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: true)
    let shareableByID = Dictionary(uniqueKeysWithValues: shareableContent.windows.map { ($0.windowID, $0) })

    for candidate in cgWindows {
        let scWindow = shareableByID[candidate.windowID]
        let payload: [String: Any] = [
            "window_id": Int(candidate.windowID),
            "owner_name": candidate.ownerName,
            "title": candidate.title,
            "bounds": [
                "x": candidate.bounds.origin.x,
                "y": candidate.bounds.origin.y,
                "width": candidate.bounds.width,
                "height": candidate.bounds.height,
            ],
            "screen_capture_kit_available": scWindow != nil,
            "point_pixel_scale": scWindow != nil ? Double(SCContentFilter(desktopIndependentWindow: scWindow!).pointPixelScale) : NSNull(),
        ]
        let data = try JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
        if let line = String(data: data, encoding: .utf8) {
            print(line)
        }
    }
}

@main
struct ScreenCaptureKitCaptureTool {
    static func main() async {
        do {
            guard #available(macOS 12.3, *) else {
                throw CaptureError.unsupportedOS("ScreenCaptureKit requires macOS 12.3 or newer")
            }

            let options = try parseArgs()
            if options.listWindows {
                try await listWindows()
                return
            }

            let calibration = try options.calibrationPath.map(loadCalibration)
            let resolvedWindow = try await resolveWindow(options: options, calibration: calibration)

            let outputPath = options.outputPath!
            let framesOutputPath = options.framesOutputPath
                ?? URL(fileURLWithPath: outputPath).deletingPathExtension().appendingPathExtension("frames.jsonl").path

            let controller = try CaptureController(
                resolvedWindow: resolvedWindow,
                eventsPath: options.eventsPath!,
                outputPath: outputPath,
                framesOutputPath: framesOutputPath,
                analysisFramesDirPath: options.analysisFramesDirPath,
                sampleHz: options.sampleHz,
                pixelThreshold: options.pixelThreshold,
                changedFractionThreshold: options.changedFractionThreshold,
                stableFrames: options.stableFrames,
                skipFrameDiffAnalysis: options.skipFrameDiffAnalysis,
                timeoutSeconds: options.timeoutSeconds,
                queueDepth: options.queueDepth
            )
            try await controller.run()
        } catch let error as CaptureError {
            fputs("error: \(error.description)\n", stderr)
            exit(1)
        } catch {
            fputs("error: \(error)\n", stderr)
            exit(1)
        }
    }
}
