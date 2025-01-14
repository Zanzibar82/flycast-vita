/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "rzip.h"
#include <zlib.h>

const u8 RZipHeader[8] = { '#', 'R', 'Z', 'I', 'P', 'v', 1, '#' };

bool RZipFile::Open(const std::string& path, bool write)
{
	verify(file == nullptr);

	file = nowide::fopen(path.c_str(), write ? "wb" : "rb");
	if (file == nullptr)
		return false;
	if (!write)
	{
		u8 header[sizeof(RZipHeader)];
		if (std::fread(header, sizeof(header), 1, file) != 1
			|| memcmp(header, RZipHeader, sizeof(header))
			|| std::fread(&maxChunkSize, sizeof(maxChunkSize), 1, file) != 1
			|| std::fread(&size, sizeof(size), 1, file) != 1)
		{
			WARN_LOG(SAVESTATE, "I/O error: Failed to validate header");
			Close();
			return false;
		}
		// savestates created on 32-bit platforms used to have a 32-bit size
		if (size >> 32 != 0)
		{
			size &= 0xffffffff;
			std::fseek(file, -4, SEEK_CUR);
		}
		chunk = new u8[maxChunkSize];
		chunkIndex = 0;
		chunkSize = 0;
		writing = false;
	}
	else
	{
		writing = true;
		size = 0;
		maxChunkSize = 1024 * 1024;

		if (std::fwrite(RZipHeader, sizeof(RZipHeader), 1, file) != 1
			|| std::fwrite(&maxChunkSize, sizeof(maxChunkSize), 1, file) != 1
			|| std::fseek(file, sizeof(size), SEEK_CUR) != 0)
		{
			WARN_LOG(SAVESTATE, "I/O error: Failed to write header");
			Close();
			return false;
		}
	}

	return true;
}

void RZipFile::Close()
{
	if (file != nullptr)
	{
		std::fclose(file);
		file = nullptr;
		if (chunk != nullptr)
		{
			delete [] chunk;
			chunk = nullptr;
		}
	}
}

size_t RZipFile::Read(void *data, size_t length)
{
	verify(file != nullptr);

	u8 *p = (u8 *)data;
	size_t rv = 0;
	while (rv < length)
	{
		if (chunkIndex == chunkSize)
		{
			chunkSize = 0;
			chunkIndex = 0;
			u32 zippedSize;
			if (std::fread(&zippedSize, sizeof(zippedSize), 1, file) != 1)
			{
				WARN_LOG(SAVESTATE, "I/O error: Failed to read the size of compressed data");
				break;
			}
			if (zippedSize == 0)
				continue;
			u8 *zipped = new u8[zippedSize];
			if (std::fread(zipped, zippedSize, 1, file) != 1)
			{
				WARN_LOG(SAVESTATE, "I/O error: Failed to read compressed data");
				delete [] zipped;
				break;
			}
			uLongf tl = maxChunkSize;
			u32 rc = uncompress(chunk, &tl, zipped, zippedSize);
			if (rc != Z_OK)
			{
				WARN_LOG(SAVESTATE, "Decompression error: %d", rc);
				delete [] zipped;
				break;
			}
			delete [] zipped;
			chunkSize = (u32)tl;
		}
		u32 l = std::min(chunkSize - chunkIndex, (u32)length - rv);
		memcpy(p, chunk + chunkIndex, l);
		p += l;
		chunkIndex += l;
		rv += l;
	}

	return rv;
}

size_t RZipFile::Skip(size_t length)
{
	verify(file != nullptr);

	size_t rv = 0;
	while (rv < length)
	{
		if (chunkIndex == chunkSize)
		{
			chunkSize = 0;
			chunkIndex = 0;
			u32 zippedSize;
			if (std::fread(&zippedSize, sizeof(zippedSize), 1, file) != 1)
			{
				WARN_LOG(SAVESTATE, "I/O error: Failed to read the size of compressed data");
				break;
			}
			if (zippedSize == 0)
				continue;
			u8 *zipped = new u8[zippedSize];
			if (std::fread(zipped, zippedSize, 1, file) != 1)
			{
				WARN_LOG(SAVESTATE, "I/O error: Failed to read compressed data");
				delete [] zipped;
				break;
			}
			uLongf tl = maxChunkSize;
			u32 rc = uncompress(chunk, &tl, zipped, zippedSize);
			if (rc != Z_OK)
			{
				WARN_LOG(SAVESTATE, "Decompression error: %d", rc);
				delete [] zipped;
				break;
			}
			delete [] zipped;
			chunkSize = (u32)tl;
		}
		u32 l = std::min(chunkSize - chunkIndex, (u32)length - rv);
		chunkIndex += l;
		rv += l;
	}

	return rv;
}

size_t RZipFile::Write(const void *data, size_t length)
{
	verify(file != nullptr);

	const u8 *p = (const u8 *)data;
	// compression output buffer must be 0.1% larger + 12 bytes
	uLongf maxZippedSize = maxChunkSize + maxChunkSize / 1000 + 12;
	u8 *zipped = new u8[maxZippedSize];
	size_t rv = 0;
	while (rv < length)
	{
		uLongf zippedSize = maxZippedSize;
		uLongf uncompressedSize = std::min(maxChunkSize, (u32)(length - rv));
		u32 rc = compress(zipped, &zippedSize, p, uncompressedSize);
		if (rc != Z_OK)
		{
			WARN_LOG(SAVESTATE, "Compression error: %d", rc);
			break;
		}
		u32 sz = (u32)zippedSize;
		if (std::fwrite(&sz, sizeof(sz), 1, file) != 1
			|| std::fwrite(zipped, zippedSize, 1, file) != 1)
		{
			WARN_LOG(SAVESTATE, "I/O error: Failed to write compressed data");
			break;
		}
		p += uncompressedSize;
		rv += uncompressedSize;
		size += uncompressedSize;
	}
	delete [] zipped;

	u64 pos = ftell(file);
	verify(std::fseek(file, sizeof(RZipHeader) + sizeof(maxChunkSize), SEEK_SET) == 0);
	verify(std::fwrite(&size, sizeof(size), 1, file) == 1);
	verify(std::fseek(file, pos, SEEK_SET) == 0);

	return rv;
}
