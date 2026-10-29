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
    uint8_t scan_mode[2];     /* 0x14: setScanModeControl: [0] multisampling, [1] the viewport scissor
                                 (the draw state's scissor 0 is applied only when it is set) */
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

/* A shader program: the shaders a material's effect picks for a technique and a pass type,
 * each an object whose virtual method +0x18 gives its CgShader (CG::ShaderProgram::bind). */
typedef struct CgShaderProgram {
    const void *vertex;   /* 0x00 */
    const void *pixel;    /* 0x08 */
    const void *hull;     /* 0x10 */
    const void *domain;   /* 0x18 */
    const void *geometry; /* 0x20 */
} CgShaderProgram;

typedef struct CgSampler {
    uint32_t ssharp[4]; /* 0x00: the GCN sampler descriptor (S#) */
} CgSampler;

typedef struct CgTexture {
    uint8_t unknown_00[0x10];
    uint32_t tsharp[8]; /* 0x10: the GCN texture descriptor (T#) */
} CgTexture;

typedef struct CgConstantBuffer {
    uint8_t unknown_00[0x18];
    uint8_t memory[0x14]; /* 0x18: a handle the context's buffer manager turns into the buffer's GPU
                             address (0x26ae840 looks it up in the manager's table; the binder then
                             calls initAsConstantBuffer with it and `size`) */
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
    const void *input_layout;            /* 0x000: the vertex layout (meshes ask their own provider
                                            for the pass's) */
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
    const void *render_targets[8];       /* 0x4e0: changing one first resolves the old one's fast clear;
                                            each holds libGnm's RenderTarget at +0x10 (its first
                                            dword CB_COLOR_BASE, the address shifted by 8) */
    const void *depth_target;            /* 0x520: changing it first decompresses the old one; holds
                                            libGnm's DepthRenderTarget at +0x10 (its third dword
                                            DB_Z_READ_BASE) */
    uint8_t unknown_528[0x40];
    float blend_color[4];                /* 0x568: setBlendColor */
    float depth_bounds[2];               /* 0x578: setDepthBoundsRange (minimum, maximum) */
    uint8_t unknown_580[0x4];
    uint8_t stencil_reference;           /* 0x584: setStencil's reference, front and back */
    uint8_t unknown_585[0x13];
    CgShaderStage stages[5];             /* 0x598: by CgStage */
} CgDrawState;

/* What CG::ResourceBinder::commitCompute binds for a dispatch (CG::GraphicsContext::dispatch and the
 * surface operations' beforeDispatch pass it): the compute shader's stage, laid out as a draw
 * stage, where a CgDrawState would end (commitCompute reads nothing before it), and the writable
 * resources after it. */
typedef struct CgComputeState {
    uint8_t unknown_000[0x1e70];
    CgShaderStage stage;                 /* 0x1e70: the compute shader's samplers, textures,
                                            constant buffers and shader */
    const void *writable[8];             /* 0x2368: writable textures and buffers; each holds its T#
                                            (or a V# in the first 16 bytes) at +0x10 */
} CgComputeState;

/* A texture of a material (0x38 bytes; FD4::MaterialTextures::bind walks the material's list at
 * +0xb0): bound to the pixel stage's slot, and to the domain stage's too for some kinds (the
 * flag below), with its UV transform gathered into a constant buffer for the same stages. */
typedef struct CgMaterialTexture {
    uint32_t slot;           /* 0x00: the texture slot (and the transform's register below 25) */
    uint8_t unknown_04[0x4];
    const void *texture;     /* 0x08: an object whose virtual method +0x18 gives the texture */
    uint32_t kind;           /* 0x10: 8 is skipped in some passes; 0xb also binds to the domain stage */
    float transform[2];      /* 0x14: the UV scale (or offset) written to the transform buffer */
    uint8_t unknown_1c[0x10];
    uint8_t always_transform; /* 0x2c */
    uint8_t unknown_2d[0x3];
    uint32_t transform_register; /* 0x30: 0xffffffff for the slot's own */
    uint32_t flags;          /* 0x34: bit 2 also binds it to the domain stage */
} CgMaterialTexture;

/* A model's index range drawn with one state (read by CG::MeshRenderer::drawMesh). */
typedef struct CgFaceSet {
    uint32_t flags;          /* 0x00: bits 24-27 select the pass the face set draws in; the sign
                                bit is tested against the pass's flags */
    uint32_t topology;       /* 0x04: as CgDrawState.topology */
    uint32_t index_format;   /* 0x08: as CgDrawState.index_format */
    uint32_t cull_mode;      /* 0x0c: picks the rasterizer state from the engine's table */
    uint32_t index_count;    /* 0x10 */
    uint32_t first_index;    /* 0x14: added to the index buffer's own first index */
    const void *index_buffer; /* 0x18: its CgBuffer at +0x10, its first index at +0x08 */
} CgFaceSet;

/* A model's vertex buffer (0x90 bytes). */
typedef struct CgModelVertexBuffer {
    uint8_t unknown_00[0x68];
    uint32_t stride;         /* 0x68: bytes */
    uint32_t base_vertex;    /* 0x6c: one for all of a mesh's buffers is passed to the draw;
                                otherwise it is folded into each buffer's offset */
    const CgBuffer *buffer;  /* 0x70 */
    uint32_t offset;         /* 0x78: bytes */
    uint8_t unknown_7c[0x14];
} CgModelVertexBuffer;

/* A model's mesh (0x48 bytes): its material, and which of the model's face sets and vertex
 * buffers it draws. */
typedef struct CgMesh {
    uint16_t material;            /* 0x00: the material's index, looked up through the model
                                     draw's material set */
    uint8_t unknown_02[0x6];
    int32_t technique;            /* 0x08: passed to the material's begin */
    uint8_t unknown_0c[0x4];
    uint32_t flags;               /* 0x10: must have the pass's bits */
    uint16_t lod_mask;            /* 0x14: the levels of detail it draws at */
    uint8_t unknown_16[0x12];
    uint16_t face_set_count;      /* 0x28 */
    uint16_t vertex_buffer_count; /* 0x2a: into CgDrawState.vertex_buffers from slot 0 */
    uint8_t unknown_2c[0x4];
    const int32_t *face_sets;     /* 0x30: indices into the model's face sets (0x20 bytes each) */
    const int32_t *vertex_buffers; /* 0x38: indices into the model's vertex buffers (0x90 bytes each) */
    const void *input_layouts;    /* 0x40: an object whose virtual method (+0x10) gives the input
                                     layout for a pass */
} CgMesh;

/* A model (CG::ModelRenderer::drawModel), as far as its draw reads it. */
typedef struct CgModel {
    uint8_t unknown_000[0xa0];
    int32_t mesh_count;           /* 0x0a0 */
    uint8_t unknown_0a4[0x4];
    const CgMesh *meshes;         /* 0x0a8 */
    uint8_t unknown_0b0[0x4];
    uint32_t lod_mask;            /* 0x0b4: the levels of detail it has (a missing one falls back
                                     to the next lower) */
    uint8_t unknown_0b8[0x80];
    const CgFaceSet *face_sets;   /* 0x138 */
    uint8_t unknown_140[0x30];
    const CgModelVertexBuffer *vertex_buffers; /* 0x170 */
} CgModel;

#define CG_AT(type, field, offset) _Static_assert(offsetof(type, field) == (offset), #type "." #field)
CG_AT(CgBuffer, address, 0x20);
CG_AT(CgBuffer, managed, 0x40);
CG_AT(CgBlendState, alpha_to_mask, 0x24);
CG_AT(CgRasterizerState, scan_mode, 0x14);
CG_AT(CgDrawState, topology, 0x008);
CG_AT(CgFaceSet, index_buffer, 0x18);
_Static_assert(sizeof(CgFaceSet) == 0x20, "CgFaceSet");
CG_AT(CgModelVertexBuffer, offset, 0x78);
_Static_assert(sizeof(CgModelVertexBuffer) == 0x90, "CgModelVertexBuffer");
CG_AT(CgMesh, input_layouts, 0x40);
CG_AT(CgMaterialTexture, transform_register, 0x30);
_Static_assert(sizeof(CgMaterialTexture) == 0x38, "CgMaterialTexture");
CG_AT(CgMesh, lod_mask, 0x14);
_Static_assert(sizeof(CgMesh) == 0x48, "CgMesh");
CG_AT(CgModel, meshes, 0x0a8);
CG_AT(CgModel, lod_mask, 0x0b4);
CG_AT(CgModel, face_sets, 0x138);
CG_AT(CgModel, vertex_buffers, 0x170);
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
CG_AT(CgComputeState, stage, 0x1e70);
CG_AT(CgComputeState, writable, 0x2368);
_Static_assert(offsetof(CgComputeState, stage.shader) == 0x2360, "the compute shader");
#undef CG_AT
