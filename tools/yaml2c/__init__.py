"""CANcestry yaml2c: static YAML-to-C code generation (Phase 8, issue #22).

This package compiles validated CANcestry v0.3.0 YAML documents (FSM files and
codec maps) into pure, static, const C definitions that map 1:1 onto the
runtime types of ``cancestry/fsm/types.h`` and ``cancestry/codec/types.h``.

The generated code needs no loader on the target: every array and string is a
``static const`` definition, so including the generated header is equivalent
to loading the YAML at run time, minus the allocation and the parser
(SW-FR-TOOL-001..004, SYS-NF-002).

Modules:
    common         errors, scalar classification and the C emitter
    expression     Python port of the load-time expression grammar check
    fsm_codegen    FSM file -> cancestry_fsm_set_t definitions
    codec_codegen  codec map -> cancestry_codec_map_t definitions
    schema         Draft 2020-12 validation of the input documents
    yaml2c         the command line interface
"""

from __future__ import annotations

__version__ = "0.3.0"
