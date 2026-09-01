# Open-Source Provenance Ledger

Record source-derived implementation decisions and deliberate code reuse here.
General familiarity does not require an entry; a specific algorithm, behavior,
constant table, or copied/adapted code does.

| Component | Repository | Revision | Source path | Use | License/notice action |
| --- | --- | --- | --- | --- | --- |
| Recompiler/runtime foundation | `https://github.com/ran-j/PS2Recomp.git` | Base revision `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7` | Whole pinned submodule | Build foundation and technical reference; locally authored reusable corrections are maintained on top and summarized in `docs/PROJECT_STATUS.md` | GPL-3.0 license retained in submodule; final distribution compliance pending |
| IPU input DMA and DMAC behavior reference | `https://github.com/PCSX2/pcsx2.git` | Tag `v2.6.3` | `pcsx2/IPU/IPUdma.cpp`, `pcsx2/IPU/IPU_Fifo.cpp`, `pcsx2/Dmac.cpp`, `pcsx2/Dmac.h` | Technical reference for the eight-QWC IPU input FIFO, partial channel progress, source-chain pause/resume, active-channel writes, and terminal completion/interrupt ordering | No source copied or adapted; implementation remains independently authored; GPL notices remain with the upstream reference |

Current PMFHL, scheduler/kernel, GS, DMA/IPU, MPEG, input, storage, and audio
changes are project-authored adaptations rather than copied PCSX2 source. If a
specific upstream algorithm or implementation is deliberately reused later,
add a separate row with its exact revision and source path before distribution.
