#!/usr/bin/env python3
"""Writes tiny_inference.onnx, the ONNX fixture of inferenceruntime_test.cpp.

A graph of two float inputs and two outputs, like the htdemucs graph but a few
hundred bytes:

    sum = x + y
    scaled = x * 2

x and y are [1, 2, n] with a dynamic n. Opset 17 and IR version 8, which every
ONNX Runtime release since 1.13 loads.

    python3 make_tiny_inference.py [output.onnx]

Needs the `onnx` package. The output is deterministic, so running it again
gives the same bytes.
"""

import sys
from pathlib import Path

import onnx
from onnx import TensorProto, helper


def build_model() -> onnx.ModelProto:
    shape = [1, 2, "n"]
    graph = helper.make_graph(
        nodes=[
            helper.make_node("Add", ["x", "y"], ["sum"]),
            helper.make_node("Mul", ["x", "two"], ["scaled"]),
        ],
        name="tiny_inference",
        inputs=[
            helper.make_tensor_value_info("x", TensorProto.FLOAT, shape),
            helper.make_tensor_value_info("y", TensorProto.FLOAT, shape),
        ],
        outputs=[
            helper.make_tensor_value_info("sum", TensorProto.FLOAT, shape),
            helper.make_tensor_value_info("scaled", TensorProto.FLOAT, shape),
        ],
        initializer=[helper.make_tensor("two", TensorProto.FLOAT, [], [2.0])],
    )
    model = helper.make_model(
        graph,
        producer_name="crate-decks-tests",
        opset_imports=[helper.make_opsetid("", 17)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model, full_check=True)
    return model


def main() -> int:
    default = Path(__file__).resolve().with_name("tiny_inference.onnx")
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    output.write_bytes(build_model().SerializeToString())
    print(f"{output}: {output.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
