#!/usr/bin/env python3
"""FastMCP bridge that uploads MIDI sequences to an Arduino UNO R4 WiFi."""

from __future__ import annotations

import argparse
import atexit
import os
import sys
from typing import Annotated, Literal

import httpx
from dotenv import load_dotenv
from fastmcp import Context, FastMCP
from pydantic import (
    BaseModel,
    Field,
    PositiveInt,
    ValidationError,
    conint,
    model_validator,
)

DEFAULT_CHANNEL = 1
MAX_EVENTS = 64
DEFAULT_TIMEOUT = 5.0

load_dotenv()


class MidiEvent(BaseModel):
    """Single step in a monophonic MIDI sequence."""

    type: Literal["note", "rest"] = Field(
        description="Event type. Use 'note' for sounding events or 'rest' for silence."
    )
    ticks: PositiveInt = Field(
        description="Duration in MIDI clock ticks (24 ticks = quarter note)."
    )
    note: conint(ge=0, le=127) | None = Field(
        default=None,
        description="MIDI note number (0-127). Required when type='note'.",
    )
    velocity: conint(ge=1, le=127) | None = Field(
        default=None,
        description="MIDI velocity (1-127). Defaults to 100 when omitted for notes.",
    )

    @model_validator(mode="after")
    def _validate_note_fields(self) -> MidiEvent:  # type: ignore[override]
        if self.type == "note":
            if self.note is None:
                raise ValueError("'note' is required when type is 'note'.")
            if self.velocity is None:
                self.velocity = 100
        else:
            # Ignore note-specific fields for rests
            self.note = None
            self.velocity = None
        return self


class LoadSequenceRequest(BaseModel):
    """Validated payload sent to the Arduino controller."""

    channel: conint(ge=1, le=16) = Field(
        default=DEFAULT_CHANNEL,
        description="MIDI channel (1-16) that the UNO should play on.",
    )
    sequence: list[MidiEvent] = Field(
        min_length=1,
        max_length=MAX_EVENTS,
        description="Ordered list of note/rest events to buffer on the UNO.",
    )


class ArduinoBridge:
    """Small HTTP client that talks to the UNO R4 REST API."""

    def __init__(self, base_url: str, *, timeout: float = DEFAULT_TIMEOUT) -> None:
        if not base_url.startswith("http://") and not base_url.startswith("https://"):
            raise ValueError(
                "arduino base URL must include the scheme, e.g. http://192.168.1.42"
            )
        self.base_url = base_url.rstrip("/")
        self._client = httpx.Client(base_url=self.base_url, timeout=timeout)

    def close(self) -> None:
        self._client.close()

    def load_sequence(self, payload: LoadSequenceRequest) -> dict:
        body = payload.model_dump(exclude_none=True)
        try:
            response = self._client.post("/sequence", json=body)
            response.raise_for_status()
        except httpx.HTTPStatusError as exc:
            detail = exc.response.text.strip()
            if exc.response.status_code == 400:
                raise ValueError(
                    f"Arduino rejected the sequence: {detail or exc}"
                ) from exc
            raise RuntimeError(
                f"Arduino returned {exc.response.status_code} {exc.response.reason_phrase}: {detail or exc}"
            ) from exc
        except httpx.HTTPError as exc:  # network / timeout issues
            raise RuntimeError(
                f"Failed to contact Arduino at {self.base_url}: {exc}"
            ) from exc
        return response.json()

    def get_status(self) -> dict:
        try:
            response = self._client.get("/status")
            response.raise_for_status()
        except httpx.HTTPError as exc:
            raise RuntimeError(f"Failed to query Arduino status: {exc}") from exc
        return response.json()


_bridge: ArduinoBridge | None = None


def get_bridge() -> ArduinoBridge:
    if _bridge is None:
        raise RuntimeError(
            "Arduino bridge has not been initialised. Call main() first."
        )
    return _bridge


server = FastMCP(
    name="uno-r4-midi-bridge",
    version="0.2.0",
    instructions=(
        "Uploads MIDI note sequences to an Arduino UNO R4 WiFi over HTTP and"
        " keeps track of its playback status."
    ),
)

SequenceArg = Annotated[
    list[MidiEvent],
    Field(
        description="Ordered list of note/rest events described in MIDI clock ticks.",
        min_length=1,
        max_length=MAX_EVENTS,
    ),
]
ChannelArg = Annotated[
    int,
    Field(
        ge=1,
        le=16,
        description="Target MIDI channel for playback (1-16).",
        examples=[1],
    ),
]


@server.tool(
    description="Buffer a MIDI sequence on the Arduino for clock-synchronised playback."
)
def load_sequence(
    sequence: SequenceArg,
    channel: ChannelArg = DEFAULT_CHANNEL,
    ctx: Context | None = None,
) -> dict:
    try:
        request = LoadSequenceRequest(channel=channel, sequence=sequence)
    except ValidationError as exc:
        raise ValueError(str(exc)) from exc

    if ctx is not None:
        ctx.info(
            f"Uploading {len(request.sequence)} events to {get_bridge().base_url} on channel {request.channel}."
        )

    result = get_bridge().load_sequence(request)
    if ctx is not None:
        ctx.debug(f"Arduino response: {result}")
    return result


@server.tool(description="Read the UNO's current transport and network status.")
def get_status(ctx: Context | None = None) -> dict:
    status = get_bridge().get_status()
    if ctx is not None:
        ctx.debug(f"Status: {status}")
    return status


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--arduino-base-url",
        default=os.environ.get("UNO_R4_BASE_URL"),
        help="Base URL for the Arduino REST API (e.g. http://192.168.1.42).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=float(os.environ.get("UNO_R4_TIMEOUT", DEFAULT_TIMEOUT)),
        help="HTTP timeout when communicating with the Arduino (seconds).",
    )
    parser.add_argument(
        "--transport",
        choices=["stdio", "sse", "streamable-http"],
        default=os.environ.get("MCP_TRANSPORT", "stdio"),
        help="Transport to expose to the MCP client.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    if not args.arduino_base_url:
        print(
            "Error: --arduino-base-url or UNO_R4_BASE_URL must be provided",
            file=sys.stderr,
        )
        return 2

    global _bridge
    _bridge = ArduinoBridge(args.arduino_base_url, timeout=args.timeout)
    atexit.register(_bridge.close)

    server.run(transport=args.transport)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
