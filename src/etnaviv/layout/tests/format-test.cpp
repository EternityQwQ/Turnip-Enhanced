/*
 * Copyright © 2026 Daniel Lang
 * SPDX-License-Identifier: MIT
 */

#include "eil_format.h"

#include "hw/common_3d.xml.h"
#include "hw/state.xml.h"
#include "hw/state_3d.xml.h"

#include <gtest/gtest.h>

TEST(FormatTable, get_entry_undefined)
{
   auto entry = eil_get_format_entry(PIPE_FORMAT_NV20);
   EXPECT_NE(nullptr, entry);
   EXPECT_FALSE(entry->has_vtx);
   EXPECT_FALSE(entry->has_tex);
   EXPECT_FALSE(entry->has_pe);
   EXPECT_EQ(EIL_EMU_NONE, entry->emulation);
   EXPECT_EQ(PIPE_FORMAT_NONE, entry->emulated);
}

#ifndef NDEBUG
TEST(FormatTable, get_entry_invalid)
{
   EXPECT_DEATH(eil_get_format_entry(static_cast<pipe_format>(PIPE_FORMAT_COUNT + 1U)), "");
   EXPECT_DEATH(eil_get_format_entry(static_cast<pipe_format>(-1)), "");
}
#endif

TEST(FormatTable, get_entry)
{
   auto entry = eil_get_format_entry(PIPE_FORMAT_R8G8B8A8_UNORM);
   EXPECT_NE(nullptr, entry);
   EXPECT_TRUE(entry->has_vtx);
   EXPECT_TRUE(entry->has_tex);
   EXPECT_TRUE(entry->has_pe);
   EXPECT_FALSE(entry->tex_ext);
   EXPECT_FALSE(entry->tex_astc);
   EXPECT_EQ(FE_DATA_TYPE_UNSIGNED_BYTE, entry->vtx);
   EXPECT_EQ(TEXTURE_FORMAT_A8R8G8B8, entry->tex);
   EXPECT_EQ(PE_FORMAT_A8R8G8B8, entry->pe);
   EXPECT_EQ(EIL_EMU_NONE, entry->emulation);
   EXPECT_EQ(PIPE_FORMAT_R8G8B8A8_UNORM, entry->emulated);
}

TEST(FormatTable, get_entry_ext)
{
   auto entry = eil_get_format_entry(PIPE_FORMAT_R8G8B8X8_SNORM);
   EXPECT_NE(nullptr, entry);
   EXPECT_TRUE(entry->has_tex);
   EXPECT_FALSE(entry->has_vtx);
   EXPECT_EQ(0, entry->vtx);
   EXPECT_TRUE(entry->tex_ext);
   EXPECT_FALSE(entry->tex_astc);
   EXPECT_EQ(EIL_EMU_NONE, entry->emulation);
   EXPECT_EQ(PIPE_FORMAT_R8G8B8X8_SNORM, entry->emulated);
}

TEST(FormatTable, get_entry_astc)
{
   auto entry = eil_get_format_entry(PIPE_FORMAT_ASTC_4x4);
   EXPECT_NE(nullptr, entry);
   EXPECT_TRUE(entry->has_tex);
   EXPECT_FALSE(entry->has_vtx);
   EXPECT_EQ(0, entry->vtx);
   EXPECT_FALSE(entry->tex_ext);
   EXPECT_TRUE(entry->tex_astc);
   EXPECT_EQ(EIL_EMU_NONE, entry->emulation);
   EXPECT_EQ(PIPE_FORMAT_ASTC_4x4, entry->emulated);
}

TEST(FormatTable, get_entry_emu_depth32f)
{
   auto entry_z32_float = eil_get_format_entry(PIPE_FORMAT_Z32_FLOAT);
   EXPECT_EQ(EIL_EMU_DEPTH32F, entry_z32_float->emulation);
   EXPECT_EQ(PIPE_FORMAT_S8_UINT_Z24_UNORM, entry_z32_float->emulated);

   auto entry_z32_float_s8x24 = eil_get_format_entry(PIPE_FORMAT_Z32_FLOAT_S8X24_UINT);
   EXPECT_EQ(EIL_EMU_DEPTH32F, entry_z32_float_s8x24->emulation);
   EXPECT_EQ(PIPE_FORMAT_S8_UINT_Z24_UNORM, entry_z32_float_s8x24->emulated);
}

TEST(FormatTable, get_entry_emu_128bit)
{
   auto rgba32_uint = eil_get_format_entry(PIPE_FORMAT_R32G32B32A32_UINT);
   EXPECT_EQ(EIL_EMU_128BIT, rgba32_uint->emulation);
   EXPECT_EQ(PIPE_FORMAT_R32G32_UINT, rgba32_uint->emulated);

   auto rgba32_sint = eil_get_format_entry(PIPE_FORMAT_R32G32B32A32_SINT);
   EXPECT_EQ(EIL_EMU_128BIT, rgba32_sint->emulation);
   EXPECT_EQ(PIPE_FORMAT_R32G32_SINT, rgba32_sint->emulated);

   auto rgba32_float = eil_get_format_entry(PIPE_FORMAT_R32G32B32A32_FLOAT);
   EXPECT_EQ(EIL_EMU_128BIT, rgba32_float->emulation);
   EXPECT_EQ(PIPE_FORMAT_R32G32_FLOAT, rgba32_float->emulated);
}

TEST(Format, is_emulated_z32f)
{
   /* not defined */
   EXPECT_FALSE(eil_format_is_emulated_z32f(PIPE_FORMAT_Z32_UNORM));

   /* not emulated */
   EXPECT_FALSE(eil_format_is_emulated_z32f(PIPE_FORMAT_R8G8B8A8_UNORM));
   EXPECT_FALSE(eil_format_is_emulated_z32f(PIPE_FORMAT_Z16_UNORM));
   EXPECT_FALSE(eil_format_is_emulated_z32f(PIPE_FORMAT_X8Z24_UNORM));

   /* emulated */
   EXPECT_TRUE(eil_format_is_emulated_z32f(PIPE_FORMAT_Z32_FLOAT));
   EXPECT_TRUE(eil_format_is_emulated_z32f(PIPE_FORMAT_Z32_FLOAT_S8X24_UINT));
}

TEST(Format, is_emulated_128bit)
{
   /* not defined */
   EXPECT_FALSE(eil_format_is_emulated_128bit(PIPE_FORMAT_R32G32B32A32_USCALED));

   /* not emulated */
   EXPECT_FALSE(eil_format_is_emulated_128bit(PIPE_FORMAT_R8G8B8A8_UNORM));
   EXPECT_FALSE(eil_format_is_emulated_128bit(PIPE_FORMAT_R32_UNORM));

   /* emulated */
   EXPECT_TRUE(eil_format_is_emulated_128bit(PIPE_FORMAT_R32G32B32A32_FLOAT));
   EXPECT_TRUE(eil_format_is_emulated_128bit(PIPE_FORMAT_R32G32B32A32_SINT));
   EXPECT_TRUE(eil_format_is_emulated_128bit(PIPE_FORMAT_R32G32B32A32_UINT));
}

TEST(Format, format_emulated)
{
   EXPECT_EQ(PIPE_FORMAT_R8G8B8A8_UNORM, eil_format_emulated(PIPE_FORMAT_R8G8B8A8_UNORM));

   /* depth32f */
   EXPECT_EQ(PIPE_FORMAT_Z16_UNORM, eil_format_emulated(PIPE_FORMAT_Z16_UNORM));
   EXPECT_EQ(PIPE_FORMAT_X8Z24_UNORM, eil_format_emulated(PIPE_FORMAT_X8Z24_UNORM));

   EXPECT_EQ(PIPE_FORMAT_S8_UINT_Z24_UNORM, eil_format_emulated(PIPE_FORMAT_Z32_FLOAT));
   EXPECT_EQ(PIPE_FORMAT_S8_UINT_Z24_UNORM, eil_format_emulated(PIPE_FORMAT_Z32_FLOAT_S8X24_UINT));

   /* 128bit */
   EXPECT_EQ(PIPE_FORMAT_R32G32B32A32_UNORM, eil_format_emulated(PIPE_FORMAT_R32G32B32A32_UNORM));

   EXPECT_EQ(PIPE_FORMAT_R32G32_UINT, eil_format_emulated(PIPE_FORMAT_R32G32B32A32_UINT));
   EXPECT_EQ(PIPE_FORMAT_R32G32_SINT, eil_format_emulated(PIPE_FORMAT_R32G32B32A32_SINT));
   EXPECT_EQ(PIPE_FORMAT_R32G32_FLOAT, eil_format_emulated(PIPE_FORMAT_R32G32B32A32_FLOAT));
}
