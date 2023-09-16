#ifndef ROTATION_PALETTE_CONSTS_INCLUDED
#define ROTATION_PALETTE_CONSTS_INCLUDED

#define PALETTE_ID_BIT_COUNT 3

#define PALETTE_ID_MULTIPLIER (1 << PALETTE_ID_BIT_COUNT)
#define PALETTE_SCALE_MULTIPLIER (1 << (8 - PALETTE_ID_BIT_COUNT))

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#define IMPOSTOR_DATA_NUM_SLICES 9

#define IMPOSTOR_DATA_SCALE 0
#define IMPOSTOR_DATA_BSP_Y__PRESHADOW 1
// spans 4 slots
#define IMPOSTOR_DATA_VERTEX_OFFSET 2

#define TRANSMITTANCE_CROWN_RAD1_DATA_OFFSET 6
#define TRANSMITTANCE_CROWN_CENTER1_DATA_OFFSET 7

#define TRANSMITTANCE_CROWN_RAD2_DATA_OFFSET 8
#define TRANSMITTANCE_CROWN_CENTER2_DATA_OFFSET 9

#define IMPOSTOR_DATA_SLICE_DATA_OFFSET 10

#define IMPOSTOR_DATA_ENTRIES_PER_SLICE 2
#define IMPOSTOR_DATA_SLICE_DATA_SIZE (IMPOSTOR_DATA_NUM_SLICES*IMPOSTOR_DATA_ENTRIES_PER_SLICE)
#define IMPOSTOR_DATA_PALETTE_OFFSET (IMPOSTOR_DATA_SLICE_DATA_OFFSET + IMPOSTOR_DATA_SLICE_DATA_SIZE)

#endif