/* annotations/structs.h: the project's layouts of the game's objects (docs/RECOMPILATION.md,
 * "Annotations"), as far as they are known, with the 1.09 executable's offsets: bytes not worked
 * out yet are padding. Names follow annotations/functions.tsv (CG: Dantelion2's CoreGraphics2).
 * Register values (the *_control fields) are libGnm's, written to the GPU as they are. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The index buffer a draw state names (read by CG::InputAssemblyState::apply). */
typedef struct CgIndexBuffer {
    uint8_t unknown_00[0x20];
    uint64_t address;  /* 0x20: the GPU address, unless `managed` */
    uint8_t unknown_28[0x18];
    uint8_t managed;   /* 0x40: the address comes from the context's buffer manager instead */
} CgIndexBuffer;

/* Blend (CG::OutputMergerState::apply); a draw state with none uses the engine's defaults. */
typedef struct CgBlendState {
    uint32_t render_target_mask; /* 0x00: setRenderTargetMask */
    uint32_t blend_control[8];   /* 0x04: setBlendControl, one per colour target */
    uint8_t alpha_to_mask;       /* 0x24: setAlphaToMaskControl */
} CgBlendState;

/* Depth and stencil (CG::OutputMergerState::apply). */
typedef struct CgDepthStencilState {
    uint32_t depth_stencil_control; /* 0x00: setDepthStencilControl */
    uint32_t stencil_op_control;    /* 0x04: setStencilOpControl */
    uint32_t stencil;               /* 0x08: setStencil's masks (bits 8-23); the reference is the draw state's */
} CgDepthStencilState;

/* Rasterizer (CG::RasterizerState::apply). */
typedef struct CgRasterizerState {
    uint32_t clip_control;    /* 0x00: setClipControl */
    uint32_t primitive_setup; /* 0x04: setPrimitiveSetup (culling, fill, polygon offset enables) */
    float polygon_offset[3];  /* 0x08: setPolygonOffsetFront/Back and Clamp */
    uint8_t scan_mode[2];     /* 0x14: setScanModeControl */
} CgRasterizerState;

typedef struct CgViewport {
    float x, y, width, height, min_depth, max_depth;
} CgViewport;

typedef struct CgScissor {
    int32_t left, top, right, bottom; /* negative values are clamped to 0 */
} CgScissor;

/* A shader stage of a draw state. The stages are in Direct3D 11's order (CgStage); the
 * vertex shader runs on GCN's LS, ES or VS stage and the domain shader on its ES or VS stage,
 * by whether tessellation and a geometry shader are on. */
typedef enum CgStage { CG_VERTEX, CG_HULL, CG_DOMAIN, CG_GEOMETRY, CG_PIXEL, CG_COMPUTE } CgStage;
typedef struct CgShaderStage {
    const void *shader;      /* 0x000: NULL when the stage is off (setActiveShaderStages) */
    uint8_t resources[0x4f0]; /* 0x008: the stage's resource tables (arrays from 0x008, 0x088,
                                 0x288 and 0x488), copied into the resource binder's own tables;
                                 which holds constant buffers, buffers, textures and samplers is
                                 not worked out yet */
} CgShaderStage;

/* What every draw method of CG::GraphicsContext takes after the context: the whole state of a
 * draw, at least 0x2360 bytes. */
typedef struct CgDrawState {
    uint8_t unknown_000[0x8];
    uint32_t topology;                   /* 0x008: the engine's own primitive type, converted for libGnm */
    uint8_t unknown_00c[0x4];
    const CgIndexBuffer *index_buffer;   /* 0x010 */
    uint32_t index_format;               /* 0x018: 0x2a for 32-bit indices, else 16-bit */
    uint32_t index_offset;               /* 0x01c: bytes, added to the index buffer's address */
    uint8_t unknown_020[0x220];
    const CgRasterizerState *rasterizer; /* 0x240: NULL for the defaults */
    CgViewport viewports[16];            /* 0x248 */
    CgScissor scissors[16];              /* 0x3c8 */
    uint8_t unknown_4c8[0x8];
    const CgBlendState *blend;                  /* 0x4d0: NULL for the defaults */
    const CgDepthStencilState *depth_stencil;   /* 0x4d8: NULL for the defaults */
    const void *render_targets[8];       /* 0x4e0: changing one first resolves the old one's fast clear */
    const void *depth_target;            /* 0x520: changing it first decompresses the old one */
    uint8_t unknown_528[0x40];
    float blend_color[4];                /* 0x568: setBlendColor */
    float depth_bounds[2];               /* 0x578: setDepthBoundsRange (minimum, maximum) */
    uint8_t unknown_580[0x4];
    uint8_t stencil_reference;           /* 0x584: setStencil's reference, front and back */
    uint8_t unknown_585[0xb];
    CgShaderStage stages[6];             /* 0x590: by CgStage */
} CgDrawState;

#define CG_AT(type, field, offset) _Static_assert(offsetof(type, field) == (offset), #type "." #field)
CG_AT(CgIndexBuffer, address, 0x20);
CG_AT(CgIndexBuffer, managed, 0x40);
CG_AT(CgBlendState, alpha_to_mask, 0x24);
CG_AT(CgRasterizerState, scan_mode, 0x14);
CG_AT(CgDrawState, topology, 0x008);
CG_AT(CgDrawState, index_buffer, 0x010);
CG_AT(CgDrawState, index_offset, 0x01c);
CG_AT(CgDrawState, rasterizer, 0x240);
CG_AT(CgDrawState, viewports, 0x248);
CG_AT(CgDrawState, scissors, 0x3c8);
CG_AT(CgDrawState, blend, 0x4d0);
CG_AT(CgDrawState, depth_stencil, 0x4d8);
CG_AT(CgDrawState, render_targets, 0x4e0);
CG_AT(CgDrawState, depth_target, 0x520);
CG_AT(CgDrawState, blend_color, 0x568);
CG_AT(CgDrawState, depth_bounds, 0x578);
CG_AT(CgDrawState, stencil_reference, 0x584);
CG_AT(CgDrawState, stages, 0x590);
_Static_assert(sizeof(CgShaderStage) == 0x4f8, "CgShaderStage");
_Static_assert(offsetof(CgDrawState, stages[CG_PIXEL]) == 0x1970, "the pixel stage");
#undef CG_AT
