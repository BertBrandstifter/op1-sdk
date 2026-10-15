// Minimal CoreMIDI sender for testing OP-1 SDK builds from the Mac.
// The OP-1 in normal mode is a USB MIDI device; notes/CCs sent here arrive at
// the hooked dispatcher (examples/fb_test draws them on screen).
//
//   swift midi_send.swift list
//   swift midi_send.swift OP-1 note 60 100        (note on, 500 ms, off)
//   swift midi_send.swift OP-1 cc 12 45
import CoreMIDI
import Foundation

let args = CommandLine.arguments
guard args.count >= 2 else {
    print("usage: midi_send.swift list | <dest> note <num> <vel> [hold-ms] | <dest> cc <num> <val>")
    exit(1)
}

func destName(_ d: MIDIEndpointRef) -> String {
    var s: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(d, kMIDIPropertyName, &s)
    return (s?.takeRetainedValue()) as String? ?? ""
}

if args[1] == "list" {
    for i in 0..<MIDIGetNumberOfDestinations() {
        print("\(i): \(destName(MIDIGetDestination(i)))")
    }
    exit(0)
}
guard args.count >= 4 else { print("missing args"); exit(1) }

var client: MIDIClientRef = 0
MIDIClientCreate("op1sdk" as CFString, nil, nil, &client)
var port: MIDIPortRef = 0
MIDIOutputPortCreate(client, "out" as CFString, &port)

var dest: MIDIEndpointRef = 0
for i in 0..<MIDIGetNumberOfDestinations() {
    let d = MIDIGetDestination(i)
    if destName(d).localizedCaseInsensitiveContains(args[1]) || "\(i)" == args[1] { dest = d; break }
}
guard dest != 0 else { print("no MIDI destination matching '\(args[1])' (try: list)"); exit(1) }

func send(_ data: [UInt8]) {
    var buf = [UInt8](repeating: 0, count: 512)
    buf.withUnsafeMutableBytes { raw in
        let pl = raw.bindMemory(to: MIDIPacketList.self).baseAddress!
        let pkt = MIDIPacketListInit(pl)
        data.withUnsafeBytes { d in
            _ = MIDIPacketListAdd(pl, raw.count, pkt, 0, data.count,
                                  d.bindMemory(to: UInt8.self).baseAddress!)
        }
        MIDISend(port, dest, pl)
    }
}

switch args[2] {
case "note":
    guard args.count >= 5 else { print("usage: <dest> note <num> <vel> [hold-ms]"); exit(1) }
    let n = UInt8(Int(args[3])!), v = UInt8(Int(args[4])!)
    let hold = args.count > 5 ? UInt32(Int(args[5])!) : 500
    send([0x90, n, v])
    usleep(hold * 1000)
    send([0x80, n, 0])
    print("note \(n) vel \(v) -> \(destName(dest))")
case "cc":
    guard args.count >= 5 else { print("usage: <dest> cc <num> <val>"); exit(1) }
    send([0xb0, UInt8(Int(args[3])!), UInt8(Int(args[4])!)])
    print("cc \(args[3]) = \(args[4]) -> \(destName(dest))")
default:
    print("unknown command '\(args[2])' (note | cc)")
    exit(1)
}
