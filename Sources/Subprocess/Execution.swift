//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2025 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

#if canImport(System)
import System
#else
@preconcurrency import SystemPackage
#endif

#if canImport(Darwin)
import Darwin
#elseif canImport(Bionic)
import Bionic
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(WinSDK)
import WinSDK
#endif

import Synchronization

/// An object that repersents a subprocess that has been
/// executed. You can use this object to send signals to the
/// child process as well as stream its output and error.
@available(macOS 9999, *)
public struct Execution<
    Output: OutputProtocol,
    Error: OutputProtocol
>: Sendable {
    /// The process identifier of the current execution
    public let processIdentifier: ProcessIdentifier

    internal let output: Output
    internal let error: Error
    internal var outputPipe: CreatedPipe
    internal var errorPipe: CreatedPipe
    internal let outputConsumptionState: LockedState<OutputConsumptionState>
#if os(Windows)
    internal let consoleBehavior: PlatformOptions.ConsoleBehavior

    init(
        processIdentifier: ProcessIdentifier,
        output: Output,
        error: Error,
        outputPipe: CreatedPipe,
        errorPipe: CreatedPipe,
        consoleBehavior: PlatformOptions.ConsoleBehavior
    ) {
        self.processIdentifier = processIdentifier
        self.output = output
        self.error = error
        self.outputPipe = outputPipe
        self.errorPipe = errorPipe
        self.outputConsumptionState = .init(.init(rawValue: 0))
        self.consoleBehavior = consoleBehavior
    }
#else
    init(
        processIdentifier: ProcessIdentifier,
        output: Output,
        error: Error,
        outputPipe: CreatedPipe,
        errorPipe: CreatedPipe,
    ) {
        self.processIdentifier = processIdentifier
        self.output = output
        self.error = error
        self.outputPipe = outputPipe
        self.errorPipe = errorPipe
        self.outputConsumptionState = .init(.init(rawValue: 0))
    }
#endif // os(Windows)
}

@available(macOS 9999, *)
extension Execution where Output == SequenceOutput {
    /// The standard output of the subprocess.
    /// Accessing this property will **fatalError** if
    /// - `.output` wasn't set to `.redirectToSequence` when the subprocess was spawned;
    /// - This property was accessed multiple times. Subprocess communicates with
    ///   parent process via pipe under the hood and each pipe can only be consumed ones.
    public var standardOutput: some AsyncSequence<SequenceOutput.Buffer, any Swift.Error> {
        let consumptionState = self.outputConsumptionState.withLock { stateStore in
            let newState = stateStore.rawValue ^ OutputConsumptionState.standardOutputConsumed.rawValue
            stateStore = .init(rawValue: newState)
            return stateStore
        }

        guard consumptionState.contains(.standardOutputConsumed),
              let fd = self.outputPipe.readFileDescriptor else {
            fatalError("The standard output has already been consumed")
        }
        return AsyncBufferSequence(fileDescriptor: fd)
    }
}

@available(macOS 9999, *)
extension Execution where Error == SequenceOutput {
    /// The standard error of the subprocess.
    /// Accessing this property will **fatalError** if
    /// - `.error` wasn't set to `.redirectToSequence` when the subprocess was spawned;
    /// - This property was accessed multiple times. Subprocess communicates with
    ///   parent process via pipe under the hood and each pipe can only be consumed ones.
    public var standardError: some AsyncSequence<SequenceOutput.Buffer, any Swift.Error> {
        let consumptionState = self.outputConsumptionState.withLock { stateStore in
            let newState = stateStore.rawValue ^ OutputConsumptionState.standardErrorConsumed.rawValue
            stateStore = .init(rawValue: newState)
            return stateStore
        }

        guard consumptionState.contains(.standardErrorConsumed),
              let fd = self.errorPipe.readFileDescriptor else {
            fatalError("The standard output has already been consumed")
        }
        return AsyncBufferSequence(fileDescriptor: fd)
    }
}

// MARK: - Output Capture
internal enum OutputCapturingState<Output: Sendable, Error: Sendable>: Sendable {
    case standardOutputCaptured(Output)
    case standardErrorCaptured(Error)
}

internal struct OutputConsumptionState: OptionSet {
    typealias RawValue = UInt8

    internal let rawValue: UInt8

    internal init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    static let standardOutputConsumed: Self = .init(rawValue: 0b0001)
    static let standardErrorConsumed: Self  = .init(rawValue: 0b0010)
}

internal typealias CapturedIOs<
    Output: Sendable, Error: Sendable
> = (standardOutput: Output, standardError: Error)

@available(macOS 9999, *)
extension Execution {
    internal func captureIOs() async throws -> CapturedIOs<
        Output.OutputType, Error.OutputType
    > {
        return try await withThrowingTaskGroup(
            of: OutputCapturingState<Output.OutputType, Error.OutputType>.self
        ) { group in
            group.addTask {
                let stdout = try await self.output.captureOutput(
                    from: self.outputPipe.readFileDescriptor
                )
                return .standardOutputCaptured(stdout)
            }
            group.addTask {
                let stderr = try await self.error.captureOutput(
                    from: self.errorPipe.readFileDescriptor
                )
                return .standardErrorCaptured(stderr)
            }

            var stdout: Output.OutputType!
            var stderror: Error.OutputType!
            while let state = try await group.next() {
                switch state {
                case .standardOutputCaptured(let output):
                    stdout = output
                case .standardErrorCaptured(let error):
                    stderror = error
                }
            }
            return (
                standardOutput: stdout,
                standardError: stderror,
            )
        }
    }
}

