/* annotations/structs.h: the project's layouts of the game's objects (docs/RECOMPILATION.md,
 * "Annotations"), as far as they are known, with the 1.09 executable's offsets: bytes not worked
 * out yet are padding. Names follow annotations/functions.tsv (CG: Dantelion2's CoreGraphics2).
 * Register values (the *_control fields) are libGnm's, written to the GPU as they are. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* A GPU buffer a draw state names as its index buffer or a vertex buffer (read by
 * CG::InputAssemblyState::apply). */
typedef struct CgBuffer {
    uint8_t unknown_00[0x20];
    uint64_t address;  /* 0x20: the GPU address, unless `managed` */
    uint8_t unknown_28[0x18];
    uint8_t managed;   /* 0x40: the address comes from the context's buffer manager instead */
} CgBuffer;

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

/* A draw state's graphics stages, in Direct3D 11's order (compute has a state of its own). The
 * vertex shader runs on GCN's LS, ES or VS stage and the domain shader on its ES or VS stage,
 * by whether tessellation and a geometry shader are on. */
typedef enum CgStage { CG_VERTEX, CG_HULL, CG_DOMAIN, CG_GEOMETRY, CG_PIXEL } CgStage;
/* A shader the draw state names (the resource binder prefetches its code into L2 before a draw). */
typedef struct CgShader {
    uint8_t unknown_000[0x100];
    const uint32_t *gnm_shader; /* 0x100: the PS4 shader's own header: the code's size in bytes in
                                   the low 23 bits of its first word */
    uint64_t code_address;      /* 0x108: the code's GPU address */
} CgShader;

typedef struct CgSampler {
    uint32_t ssharp[4]; /* 0x00: the GCN sampler descriptor (S#) */
} CgSampler;

typedef struct CgTexture {
    uint8_t unknown_00[0x10];
    uint32_t tsharp[8]; /* 0x10: the GCN texture descriptor (T#) */
} CgTexture;

typedef struct CgConstantBuffer {
    uint8_t unknown_00[0x18];
    uint8_t memory[0x14]; /* 0x18: what the context turns into the buffer's GPU address */
    uint32_t size;        /* 0x2c: bytes, for initAsConstantBuffer */
} CgConstantBuffer;

/* The resource binder sets each table that changed: samplers and textures are copied into its
 * own descriptor tables, constant buffers become V#s (initAsConstantBuffer); a missing sampler
 * is the engine's default one. */
typedef struct CgShaderStage {
    const CgSampler *samplers[16];                /* 0x000 */
    const CgTexture *textures[64];                /* 0x080 */
    const CgTexture *more_textures[64];           /* 0x280: placed after the first 64 in the binder's table */
    const CgConstantBuffer *constant_buffers[14]; /* 0x480 */
    const CgShader *shader;                       /* 0x4f0: NULL when the stage is off (setActiveShaderStages) */
} CgShaderStage;

/* What every draw method of CG::GraphicsContext takes after the context: the whole state of a
 * draw (0x1e70 bytes), modelled on Direct3D 11 (its stages, slot counts and DXGI formats).
 * The graphics context holds one of its own at +0xb730 (its immediate draws fill it). */
typedef struct CgDrawState {
    uint8_t unknown_000[0x8];
    uint32_t topology;                   /* 0x008: Direct3D 11's order without its gaps: 0 none, 1-5 point
                                            list to triangle strip, 6-9 the adjacency lists, 10-41
                                            patch lists of 1-32 control points */
    uint8_t unknown_00c[0x4];
    const CgBuffer *index_buffer;        /* 0x010 */
    uint32_t index_format;               /* 0x018: DXGI: 0x2a (R32_UINT) or 0x39 (R16_UINT) */
    uint32_t index_offset;               /* 0x01c: bytes, added to the index buffer's address */
    const CgBuffer *vertex_buffers[32];  /* 0x020: 32 slots by the space they take and Direct3D 11's
                                            count (slot 0 seen written) */
    uint32_t vertex_strides[32];         /* 0x120: bytes */
    uint32_t vertex_offsets[32];         /* 0x1a0: bytes */
    uint8_t unknown_220[0x20];
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
    uint8_t unknown_585[0x13];
    CgShaderStage stages[5];             /* 0x598: by CgStage */
} CgDrawState;

#define CG_AT(type, field, offset) _Static_assert(offsetof(type, field) == (offset), #type "." #field)
CG_AT(CgBuffer, address, 0x20);
CG_AT(CgBuffer, managed, 0x40);
CG_AT(CgBlendState, alpha_to_mask, 0x24);
CG_AT(CgRasterizerState, scan_mode, 0x14);
CG_AT(CgDrawState, topology, 0x008);
CG_AT(CgDrawState, index_buffer, 0x010);
CG_AT(CgDrawState, index_offset, 0x01c);
CG_AT(CgDrawState, vertex_buffers, 0x020);
CG_AT(CgDrawState, vertex_strides, 0x120);
CG_AT(CgDrawState, vertex_offsets, 0x1a0);
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
CG_AT(CgDrawState, stages, 0x598);
_Static_assert(sizeof(CgShaderStage) == 0x4f8, "CgShaderStage");
CG_AT(CgShaderStage, textures, 0x080);
CG_AT(CgShaderStage, more_textures, 0x280);
CG_AT(CgShaderStage, constant_buffers, 0x480);
CG_AT(CgShaderStage, shader, 0x4f0);
CG_AT(CgTexture, tsharp, 0x10);
CG_AT(CgShader, code_address, 0x108);
CG_AT(CgConstantBuffer, size, 0x2c);
_Static_assert(offsetof(CgDrawState, stages[CG_PIXEL].shader) == 0x1e68, "the pixel shader");
_Static_assert(offsetof(CgDrawState, stages[CG_GEOMETRY].shader) == 0x1970, "the geometry shader");
_Static_assert(sizeof(CgDrawState) == 0x1e70, "CgDrawState");
#undef CG_AT
