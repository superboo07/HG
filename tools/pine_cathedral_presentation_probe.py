#!/usr/bin/env python3
"""Cold-boot PINE probe for the cathedral movie presentation/frame-pool path."""

from __future__ import annotations

import argparse
import json
import socket
import time

from pine_movie_ring_probe import MENU_OBJECT, MENU_VTABLE, read32s
from pine_pad_pulse import write_and_read16


PAD_ADDRESS = 0x004F1550

# Exact addresses are scoped to the verified SLUS_210.75 build recorded in
# docs/PROJECT_STATUS.md. They intentionally cover only the demonstrated
# presentation selector and decoded-frame pool.
ADDRESSES = {
    "root_state": 0x01081488,
    "presentation_clock": 0x010823F4,
    "presentation_scale": 0x010823F8,
    "presentation_stride": 0x01081EF4,
    "presentation_event_gate": 0x01081E7C,
    "presentation_mode": 0x01081E80,
    "clock_method_selected": 0x01082170,
    "clock_update_gate": 0x01081F60,
    "movie_object_enabled": 0x0108148C,
    "system_clock": 0x003EA108,
    "movie_manager_singleton": 0x003EA150,
    "active_mpeg_handle": 0x001717BC,
    "update_slot0": 0x003EA154,
    "update_slot1": 0x003EA158,
    "update_slot2": 0x003EA15C,
    "update_slot3": 0x003EA160,
    "update_slot4": 0x003EA164,
    "update_slot5": 0x003EA168,
    "update_slot6": 0x003EA16C,
    "update_slot7": 0x003EA170,
    "mpeg_sample_counter": 0x003B7230,
    "mpeg_clock_ready": 0x003C378C,
    "mpeg_clock_value": 0x003C37B0,
    "mpeg_clock_scale": 0x003C37D8,
    "mpeg_clock_delta": 0x003C37DC,
    "mpeg_object_mode": 0x003B72FD,
    "mpeg_object_source": 0x003B7300,
    "mpeg_object_aux": 0x003B7308,
    # Exact state-2 predicate inputs used by 0x001D5710.  The middleware
    # compares the 0x001CD0E0 result with twice +0x48, then compares the
    # source-child metric returned by 0x001CE6D8 with 0x001CD0F8.  The
    # aligned word at +0x70 captures event bytes +0x70 through +0x73.
    "mpeg_object_threshold": 0x003B7344,
    "mpeg_object_event_flags": 0x003B736C,
    "mpeg_object_bias": 0x003B7384,
    "mpeg_object_accum": 0x003B7398,
    "mpeg_object_sample_base": 0x003B739C,
    "mpeg_object_offset_a4": 0x003B73A0,
    "mpeg_object_offset_a8": 0x003B73A4,
    "mpeg_source_word0": 0x003BFCC0,
    "mpeg_source_child": 0x003BFCC4,
    "mpeg_source_segment": 0x003BFCD4,
    "mpeg_source_segment_length": 0x003BFCD8,
    "mpeg_source_position": 0x003BFCEC,
    # Work-record state used by 0x001CDB30 -> 0x001C5E00. Native cycles
    # 0 -> 1 -> 2 -> 3 -> 0 while PCM is available, then remains at zero.
    "mpeg_source_work_state": 0x003B36C8,
    "mpeg_source_work_handle": 0x003B36CC,
    "mpeg_source_work_metric": 0x003B36D4,
    "mpeg_source_work_acquire": 0x003B373C,
    "mpeg_source_work_complete": 0x003B3744,
    "mpeg_source_work_destination": 0x003B3748,
    "mpeg_source_work_chunk": 0x003B3754,
    "mpeg_source_work_written": 0x003B3758,
    "audio_cursor_object": 0x003CEBC4,
    "audio_cursor_child": 0x003CEBCC,
    "audio_cursor_expected": 0x003CEBF0,
    "audio_cursor_count": 0x003CC700,
    "audio_cursor_mode": 0x003CC708,
    "audio_channel_object": 0x003D4040,
    "audio_queue_head": 0x003D4058,
    "audio_queue_node0_next": 0x003CCC60,
    "audio_queue_node0_flags": 0x003CCC64,
    "audio_queue_node0_position": 0x003CCC68,
    "audio_queue_node0_size": 0x003CCC6C,
    "audio_queue_node1_next": 0x003CCC70,
    "audio_queue_node1_flags": 0x003CCC74,
    "audio_queue_node1_position": 0x003CCC78,
    "audio_queue_node1_size": 0x003CCC7C,
    "audio_queue_node2_next": 0x003CCC80,
    "audio_queue_node2_flags": 0x003CCC84,
    "audio_queue_node2_position": 0x003CCC88,
    "audio_queue_node2_size": 0x003CCC8C,
    "audio_queue_node3_next": 0x003CCC90,
    "audio_queue_node3_flags": 0x003CCC94,
    "audio_queue_node3_position": 0x003CCC98,
    "audio_queue_node3_size": 0x003CCC9C,
    "audio_destination_object": 0x003D4070,
    "audio_destination_head": 0x003D4088,
    "audio_destination_node0_next": 0x003CCD60,
    "audio_destination_node0_flags": 0x003CCD64,
    "audio_destination_node0_position": 0x003CCD68,
    "audio_destination_node0_size": 0x003CCD6C,
    "audio_destination_node1_next": 0x003CCD70,
    "audio_destination_node1_flags": 0x003CCD74,
    "audio_destination_node1_position": 0x003CCD78,
    "audio_destination_node1_size": 0x003CCD7C,
    # Shared EE/IOP command-status records serviced by 0x001DABC0 ->
    # 0x001DA7F0. The callbacks are part of the ordinary guest transport;
    # these fields are observed only and are never advanced by this probe.
    "transport_response_state": 0x003C4470,
    "transport_response_dma_id": 0x003C4474,
    "transport_response_last_sequence": 0x003C4478,
    "transport_response_buffer": 0x003C447C,
    "transport_response_buffer_bytes": 0x003C4480,
    "transport_response_tail_alias": 0x003C4484,
    "transport_response_iop_address": 0x003C4488,
    "transport_response_transfer_bytes": 0x003C448C,
    "transport_response_parser": 0x003C4490,
    "transport_response_parser_parameter": 0x003C4494,
    "transport_response_builder": 0x003C4498,
    "transport_response_builder_parameter": 0x003C449C,
    "transport_command_state": 0x003C44B4,
    "transport_command_dma_id": 0x003C44B8,
    "transport_command_last_sequence": 0x003C44BC,
    "transport_command_buffer": 0x003C44C0,
    "transport_command_buffer_bytes": 0x003C44C4,
    "transport_command_tail_alias": 0x003C44C8,
    "transport_command_iop_address": 0x003C44CC,
    "transport_command_transfer_bytes": 0x003C44D0,
    "transport_command_completion": 0x003C44D4,
    "transport_command_completion_parameter": 0x003C44D8,
    "transport_command_builder": 0x003C44DC,
    "transport_command_builder_parameter": 0x003C44E0,
    "audio_ring_object": 0x003D0128,
    "audio_ring_cursor": 0x003D0134,
    "audio_ring_remainder": 0x003D0138,
    "audio_ring_callback": 0x003D0160,
    "audio_ring_callback_object": 0x003D0164,
    "audio_ring1_object": 0x003D0168,
    "audio_ring1_cursor": 0x003D0174,
    "audio_ring1_remainder": 0x003D0178,
    "audio_ring1_callback": 0x003D01A0,
    "audio_ring1_callback_object": 0x003D01A4,
    "audio_ring2_object": 0x003D01A8,
    "audio_ring2_cursor": 0x003D01B4,
    "audio_ring2_remainder": 0x003D01B8,
    "audio_ring2_callback": 0x003D01E0,
    "audio_ring2_callback_object": 0x003D01E4,
    "cathedral_ring_object": 0x003D01E8,
    "cathedral_ring_cursor": 0x003D01F4,
    "cathedral_ring_remainder": 0x003D01F8,
    "cathedral_ring_callback": 0x003D0220,
    "cathedral_ring_callback_object": 0x003D0224,
    "audio_chunk_record0_state": 0x003BAA80,
    "audio_chunk_record0_size": 0x003BAA84,
    "audio_chunk_record1_state": 0x003BAA90,
    "audio_chunk_record1_size": 0x003BAA94,
    "audio_chunk_record2_state": 0x003BAAA0,
    "audio_chunk_record2_size": 0x003BAAA4,
    "snddrv_command_word0": 0x01970D40,
    "snddrv_command_word1": 0x01970D44,
    "snddrv_command_word2": 0x01970D48,
    "snddrv_command_word3": 0x01970D4C,
    "snddrv_command_word4": 0x01970D50,
    "snddrv_command_word5": 0x01970D54,
    "snddrv_command_word6": 0x01970D58,
    "snddrv_command_word7": 0x01970D5C,
    "snddrv_response": 0x01973EC0,
    "mpeg_timestamp_num": 0x003BAA10,
    "mpeg_timestamp_den": 0x003BAA14,
    "channel2_record": 0x01082830,
    "channel2_mode": 0x01082834,
    "channel2_event": 0x0108287C,
    "event_class6_table": 0x0108350C,
    "event_class6_state": 0x01083510,
    # 0x0024E5F8/0x0024E7B0 consume the event records referenced here.
    # The queue header stores its mode at +0x7C, record count at +0x178,
    # and 0xE8-byte records beginning at +0x180.
    "event_queue": 0x010833F8,
    # Scene/action lifecycle used by the post-New Game transition.  These
    # addresses are observed alongside the movie fields so a Cross-only cold
    # boot can be compared with the native runner without the stale two-Up
    # menu sequence used by pine_movie_ring_probe.py.
    "scene_action_global": 0x0044E958,
    "scene_owner": 0x0044E960,
    "scene_queue_slot0": 0x00888404,
    "scene_queue_slot1": 0x00888408,
    "scene_queue_slot2": 0x0088840C,
    "scene_queue_slot3": 0x00888410,
    "scene_container_storage": 0x0196144C,
    "scene_container_count": 0x01961450,
    "scene_container_state": 0x01961454,
    "scene_action_vtable": 0x009C9140,
    "scene_action_state_words": 0x009C9150,
    "input_edge": 0x0047E37C,
    # Channel 3 is selected by root+0x20D0 during the stalled second stream.
    # 0x00244A58 indexes the 0x74-byte record at root+0x1308, tests +0x04
    # before dispatching through +0x4C, and the resulting event is then
    # validated through its +0x14/+0x18 fields by 0x00255780.
    "channel3_record_type": 0x010828A4,
    "channel3_record_gate": 0x010828A8,
    "handle3_status": 0x010828AC,
    "channel3_event_word14": 0x010828B8,
    "channel3_event_word18": 0x010828BC,
    "channel3_method_index": 0x010828F0,
    "handle4_status": 0x01082920,
    # First event record returned for channel 3 by 0x00244A58 during the
    # opening stream.  0x00255780 passes its +0x14/+0x18 pair to 0x00255940.
    "channel3_selected_event_word14": 0x01082C14,
    "channel3_selected_event_word18": 0x01082C18,
    "slot0_state": 0x01083960,
    "slot1_state": 0x01083A48,
    "slot2_state": 0x01083B30,
    "slot3_state": 0x01083C18,
    "slot6_ready": 0x01083500,
    "slot7_ready": 0x01083544,
    "frame_pool_count": 0x01083958,
    "active_channel": 0x01083510,
    "interp_flags0": 0x01082458,
    "interp_flags1": 0x0108245C,
    "interp_flags2": 0x01082460,
    "interp_flags3": 0x01082464,
    "interp_limit_num": 0x01082580,
    "interp_limit_den": 0x01082588,
    "interp_step_num": 0x01082590,
    "interp_step_den": 0x01082598,
    "interp_bias_num": 0x010825A0,
    "interp_bias_den": 0x010825A8,
    "interp_base0_num": 0x010825B0,
    "interp_base0_den": 0x010825B8,
    "interp_base1_num": 0x010825C0,
    "interp_base1_den": 0x010825C8,
    "interp_anchor_num": 0x010825D0,
    "interp_anchor_den": 0x010825D8,
    "interp_threshold": 0x010825E0,
    "interp_counter0": 0x010825E8,
    "interp_counter1": 0x010825EC,
    "interp_mode": 0x010825F8,
    "interp_window_lo": 0x01082604,
    "interp_window_hi": 0x01082608,
    "interp_window2_lo": 0x0108260C,
    "interp_window2_hi": 0x01082610,
}


def parse_u32(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in 32 bits")
    return parsed


def pulse(connection: socket.socket, buttons: int, active: float = 0.08) -> None:
    active_low = 0xFFFF & ~buttons
    deadline = time.monotonic() + active
    while time.monotonic() < deadline:
        write_and_read16(connection, PAD_ADDRESS, active_low)
        time.sleep(0.002)
    deadline = time.monotonic() + 0.20
    while time.monotonic() < deadline:
        write_and_read16(connection, PAD_ADDRESS, 0xFFFF)
        time.sleep(0.002)


def connect(host: str, slot: int, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    while True:
        try:
            return socket.create_connection((host, slot), timeout=1.0)
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit("PINE endpoint did not become ready")
            time.sleep(0.1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--slot", type=int, default=28012)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    parser.add_argument("--menu-timeout", type=float, default=60.0)
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--interval", type=float, default=0.002)
    parser.add_argument(
        "--start-pulse-after",
        type=float,
        help="pulse Start this many seconds after the selected movie root becomes active",
    )
    parser.add_argument(
        "--start-pulse-seconds",
        type=float,
        default=0.08,
        help="duration of the optional in-probe Start pulse (default: 0.08)",
    )
    parser.add_argument(
        "--dereference-source",
        action="store_true",
        help="include four words from the current MPEG-source segment pointer",
    )
    parser.add_argument(
        "--dereference-transport",
        action="store_true",
        help=(
            "include the configured prefix of each live transport buffer; "
            "word 15 is the dynamically allocated sequence field"
        ),
    )
    parser.add_argument(
        "--transport-bytes",
        type=parse_u32,
        default=0x40,
        help="transport buffer prefix to capture when dereferencing (default: 0x40)",
    )
    parser.add_argument(
        "--dereference-response-objects",
        action="store_true",
        help="follow response record handles through their callback object and vtable",
    )
    parser.add_argument(
        "--dereference-event-queue",
        action="store_true",
        help="follow the class-6 event queue and include each record's first 0x40 bytes",
    )
    parser.add_argument(
        "--target",
        choices=("cathedral", "opening"),
        default="cathedral",
        help=(
            "cathedral submits no input and waits for the second movie-root "
            "lifecycle; opening waits for the menu and confirms New Game"
        ),
    )
    args = parser.parse_args()
    if args.duration <= 0 or args.interval < 0:
        parser.error("duration must be positive and interval must be non-negative")
    if args.start_pulse_after is not None and args.start_pulse_after < 0:
        parser.error("--start-pulse-after must be non-negative")
    if args.start_pulse_seconds <= 0:
        parser.error("--start-pulse-seconds must be positive")
    if args.transport_bytes < 0x40 or args.transport_bytes > 0x880 or args.transport_bytes % 4:
        parser.error("--transport-bytes must be a 4-byte multiple from 0x40 through 0x880")
    if args.dereference_response_objects and not args.dereference_transport:
        parser.error("--dereference-response-objects requires --dereference-transport")

    addresses = list(ADDRESSES.values())
    with connect(args.host, args.slot, args.connect_timeout) as connection:
        connection.settimeout(2.0)
        deadline = time.monotonic() + args.menu_timeout
        if args.target == "cathedral":
            # The first movie uses update object 0x01007E40. The cathedral has
            # its own verified root at 0x01081440, so its first nonzero state is
            # an exact, input-free stream trigger.
            while read32s(connection, [ADDRESSES["root_state"]])[0] == 0:
                if time.monotonic() >= deadline:
                    raise SystemExit("cathedral movie root did not become active")
                time.sleep(0.002)
            start = time.monotonic()
        else:
            while read32s(connection, [MENU_OBJECT])[0] != MENU_VTABLE:
                if time.monotonic() >= deadline:
                    raise SystemExit("main menu did not become ready")
                time.sleep(0.02)
            start = time.monotonic()
            pulse(connection, 0x4000)

        last_values: list[int] | None = None
        start_pulse_released = False
        sample = 0
        deadline = start + args.duration
        while time.monotonic() < deadline:
            elapsed = time.monotonic() - start
            if args.start_pulse_after is not None:
                if args.start_pulse_after <= elapsed < (
                    args.start_pulse_after + args.start_pulse_seconds
                ):
                    write_and_read16(connection, PAD_ADDRESS, 0xFFFF & ~0x0008)
                elif elapsed >= args.start_pulse_after + args.start_pulse_seconds and not start_pulse_released:
                    write_and_read16(connection, PAD_ADDRESS, 0xFFFF)
                    start_pulse_released = True
            values = read32s(connection, addresses)
            if values != last_values:
                source_words: list[str] | None = None
                if args.dereference_source:
                    source_pointer = values[list(ADDRESSES).index("mpeg_source_segment")]
                    if source_pointer != 0:
                        source_words = [
                            f"0x{value:08X}"
                            for value in read32s(
                                connection,
                                [source_pointer + offset for offset in range(0, 16, 4)],
                            )
                        ]
                transport_words: dict[str, list[str]] | None = None
                response_objects: list[dict[str, object]] | None = None
                event_queue: dict[str, object] | None = None
                if args.dereference_event_queue:
                    queue_pointer = values[list(ADDRESSES).index("event_queue")] & 0x1FFFFFFF
                    if queue_pointer != 0:
                        queue_mode, queue_count = read32s(
                            connection,
                            [queue_pointer + 0x7C, queue_pointer + 0x178],
                        )
                        bounded_count = min(queue_count, 16)
                        event_queue = {
                            "pointer": f"0x{queue_pointer:08X}",
                            "mode": f"0x{queue_mode:08X}",
                            "count": queue_count,
                            "records": [
                                [
                                    f"0x{value:08X}"
                                    for value in read32s(
                                        connection,
                                        [
                                            queue_pointer + 0x180 + record * 0xE8 + offset
                                            for offset in range(0, 0x40, 4)
                                        ],
                                    )
                                ]
                                for record in range(bounded_count)
                            ],
                        }
                if args.dereference_transport:
                    transport_words = {}
                    for name in (
                        "transport_response_buffer",
                        "transport_response_tail_alias",
                        "transport_command_buffer",
                        "transport_command_tail_alias",
                    ):
                        pointer = values[list(ADDRESSES).index(name)] & 0x1FFFFFFF
                        if pointer != 0:
                            transport_words[name] = [
                                f"0x{value:08X}"
                                for value in read32s(
                                    connection,
                                    [
                                        pointer + offset
                                        for offset in range(0, args.transport_bytes, 4)
                                    ],
                                )
                            ]
                    if args.dereference_response_objects:
                        response_objects = []
                        response_pointer = (
                            values[list(ADDRESSES).index("transport_response_buffer")]
                            & 0x1FFFFFFF
                        )
                        if response_pointer != 0:
                            response_count = min(read32s(connection, [response_pointer])[0], 8)
                            for record in range(response_count):
                                record_pointer = response_pointer + 0x10 + record * 0x10
                                record_words = read32s(
                                    connection,
                                    [record_pointer + offset for offset in range(0, 0x10, 4)],
                                )
                                handle = record_words[1] & 0x1FFFFFFF
                                handle_words = (
                                    read32s(
                                        connection,
                                        [handle + offset for offset in range(0, 0x10, 4)],
                                    )
                                    if handle != 0
                                    else []
                                )
                                callback = handle_words[1] & 0x1FFFFFFF if handle_words else 0
                                callback_words = (
                                    read32s(
                                        connection,
                                        [callback + offset for offset in range(0, 0x10, 4)],
                                    )
                                    if callback != 0
                                    else []
                                )
                                vtable = callback_words[0] & 0x1FFFFFFF if callback_words else 0
                                vtable_words = (
                                    read32s(
                                        connection,
                                        [vtable + offset for offset in range(0, 0x30, 4)],
                                    )
                                    if vtable != 0
                                    else []
                                )
                                response_objects.append(
                                    {
                                        "record": [f"0x{value:08X}" for value in record_words],
                                        "handle": [f"0x{value:08X}" for value in handle_words],
                                        "callback": [f"0x{value:08X}" for value in callback_words],
                                        "vtable": [f"0x{value:08X}" for value in vtable_words],
                                    }
                                )
                print(
                    json.dumps(
                        {
                            "sample": sample,
                            "elapsed_seconds": round(time.monotonic() - start, 6),
                            "values": {
                                name: f"0x{value:08X}"
                                for name, value in zip(ADDRESSES, values, strict=True)
                            },
                            **(
                                {"mpeg_source_segment_words": source_words}
                                if source_words is not None
                                else {}
                            ),
                            **(
                                {"transport_words": transport_words}
                                if transport_words is not None
                                else {}
                            ),
                            **(
                                {"transport_response_objects": response_objects}
                                if response_objects is not None
                                else {}
                            ),
                            **(
                                {"event_queue": event_queue}
                                if event_queue is not None
                                else {}
                            ),
                        },
                        separators=(",", ":"),
                    ),
                    flush=True,
                )
                last_values = values
            sample += 1
            if args.interval:
                time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
