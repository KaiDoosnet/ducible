/*
 * Copyright (c) 2016 Jason White
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * This file contains the core logic for parsing the PE file, finding the places
 * in the PE file that need to be overwritten, and overwriting those places.
 *
 * In general, it works like this:
 *
 *  1. Map the PE file into memory. Since we are not changing the size of the
 *     file, this is much more efficient than simply loading the whole file into
 *     memory the naive way. To write to the file, we simply set values in the
 *     appropriate memory locations.
 *
 *  2. Parse the PE headers. Until we get to the optional header, there are no
 *     differences between the PE and PE+ formats (32- and 64-bit images). We
 *     must parse the optional header differently depending on which format the
 *     file is.
 *
 *  3. After the main headers are parsed, we start marking regions in the file
 *     that need to be patched. Note that we do not overwrite these locations
 *     immediately because there is still more parsing to do. If the parsing
 *     fails, we do not want to end up in an inconsistent state. The potential
 *     for a partial success/failure state should be minimized as much as
 *     possible. Thus, we do not apply the patches until the very end. The main
 *     places to patch include:
 *
 *     a. Timestamps that occur in the main headers. We patch all of these with
 *        a semi-arbitrary timestamp of Jan 1, 2010, 0:00:00 GMT. We cannot use
 *        0 as that has a special meaning. Rather than being inconsistent with
 *        prior work, we use the same one as Google's zap_timestamp utility.
 *     b. Timestamps in the data directories. There are three of them with
 *        non-reproducible data: IMAGE_EXPORT_DIRECTORY,
 *        IMAGE_RESOURCE_DIRECTORY, and IMAGE_DEBUG_DIRECTORY. The tricky one is
 *        the debug directory. In addition to a timestamp, this includes a
 *        signature to match the PE file with the PDB file. We patch this with
 *        an MD5 checksum of the PE file, skipping over the patched areas. This
 *        checksum is calculated after all of the patches are added. When the
 *        patches are applied, this is what will be set.
 *
 *  4. Finally, the patches are applied.
 *
 * References:
 * - https://msdn.microsoft.com/en-us/library/ms809762.aspx
 * - http://www.debuginfo.com/articles/debuginfomatch.html
 * - https://github.com/google/syzygy/
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <cstring>
#include <iostream>
#include <vector>

#include "patch_image.h"

#include "patches.h"
#include "pe_file.h"
#include "file.h"

#include "msf.h"
#include "msf_stream.h"
#include "pdb.h"

#include "memmap.h"
#include "md5.h"

namespace {

/**
 * There are 0 or more debug data directories. We need to patch the timestamp in
 * all of them.
 */
template<typename OptHeader>
void patchDebugDataDirectories(const PEFile& pe, Patches& patches,
        const OptHeader* opt) {

    size_t debugDirCount;
    auto dir = pe.getDebugDataDirs(opt, debugDirCount);

    // Information about the PDB.
    const CV_INFO_PDB70* cvInfo = NULL;

    // Patch all of the debug data directories. Note that, at most, one of these
    // will be of type IMAGE_DEBUG_TYPE_CODEVIEW. We will use this to also patch
    // the PDB.
    for (size_t i = 0; i < debugDirCount; ++i) {
        if (dir->TimeDateStamp != 0)
            patches.add(&dir->TimeDateStamp, &pe.timestamp,
                    "IMAGE_DEBUG_DIRECTORY.TimeDateStamp");

        if (dir->Type == IMAGE_DEBUG_TYPE_CODEVIEW) {
            if (cvInfo)
                throw InvalidImage("found multiple CodeView debug entries");

            cvInfo = (const CV_INFO_PDB70*)(pe.buf + dir->PointerToRawData);

            if (!pe.isValidRef(cvInfo))
                throw InvalidImage("invalid CodeView debug entry location");
        }

        ++dir;
    }

    if (cvInfo) {
        if (cvInfo->CvSignature != CV_INFO_SIGNATURE_PDB70)
            throw InvalidImage("unsupported PDB format, only version 7.0 is supported");

        patches.add(&cvInfo->Signature, &pe.pdbSignature, "PDB Signature");
        patches.add(&cvInfo->Age, &pe.pdbAge, "PDB Age");
    }
}

/**
 * Patches the image based on the optional header type. The optional header can
 * be either 32- or 64-bit.
 */
template<typename T>
void patchOptionalHeader(const PEFile& pe, Patches& patches, const T* optional) {

    patches.add(&optional->CheckSum, &pe.timestamp,
            "OptionalHeader.CheckSum");

    // Patch exports directory timestamp
    if (auto dir = pe.getDataDir<IMAGE_EXPORT_DIRECTORY>(optional,
                IMAGE_DIRECTORY_ENTRY_EXPORT)) {
        patches.add(&dir->TimeDateStamp, &pe.timestamp,
                "IMAGE_EXPORT_DIRECTORY.TimeDateStamp");
    }

    // Patch resource directory timestamp
    if (auto dir = pe.getDataDir<IMAGE_RESOURCE_DIRECTORY>(optional,
                IMAGE_DIRECTORY_ENTRY_RESOURCE)) {
        patches.add(&dir->TimeDateStamp, &pe.timestamp,
                "IMAGE_RESOURCE_DIRECTORY.TimeDateStamp");
    }

    // Patch the debug directories
    patchDebugDataDirectories(pe, patches, optional);
}

/**
 * Calculates the checksum for the PE image, skipping over patched areas. This
 * is used to replace the PDB signature with something that is deterministic.
 *
 * The list of patches is assumed to be sorted.
 *
 * Note that this uses the MD5 hashing algorithm, but any 128-bit hashing
 * algorithm could be used instead. It might be a good idea to use a hashing
 * algorithm with better distribution to avoid collisions. MurmurHash3 could be
 * a good choice, but it can't incrementally hash chunks of data.
 */
void calculateChecksum(const uint8_t* buf, const size_t length,
        const std::vector<Patch>& patches, uint8_t output[16]) {

    size_t pos = 0;

    md5_context ctx;
    md5_starts(&ctx);

    // Take the checksum of the regions between the patches to ensure a
    // deterministic file checksum. Since the patches are sorted, we iterate
    // over the file sequentially.
    for (auto&& patch: patches) {
        // Hash everything up to the patch
        md5_update(&ctx, buf + pos, patch.offset - pos);

        // Skip past the patch
        pos = patch.offset + patch.length;
    }

    // Get everything after the last patch
    md5_update(&ctx, buf + pos, length - pos);

    md5_finish(&ctx, output);
}

/**
 * Compares the PE and PDB signatures to see if they match.
 */
bool matchingSignatures(const CV_INFO_PDB70& pdbInfo,
                        const PdbStream70& pdbHeader) {
    if (pdbInfo.Age != pdbHeader.age ||
        memcmp(pdbInfo.Signature, pdbHeader.sig70, sizeof(pdbHeader.sig70)) != 0
        ) {
        return false;
    }

    return true;
}

template<typename CharT>
constexpr CharT tmpSuffix[];

template<> constexpr char tmpSuffix<char>[] = ".tmp";
template<> constexpr wchar_t tmpSuffix<wchar_t>[] = L".tmp";

/**
 * Returns a temporary PDB path name. The PDB will be written here first and
 * then renamed to the original after everything succeeds.
 */
template<typename CharT>
std::basic_string<CharT> getTempPdbPath(const CharT* pdbPath) {
    std::basic_string<CharT> temp(pdbPath);
    temp.append(tmpSuffix<CharT>);
    return temp;
}

/**
 * Patches a PDB file.
 */
template<typename CharT>
void patchPDB(const CharT* pdbPath, const CV_INFO_PDB70* pdbInfo,
        const uint8_t signature[16], bool dryrun) {

    auto pdb = openFile(pdbPath, FileMode<CharT>::readExisting);
    if (!pdb) {
        throw std::system_error(errno, std::system_category(),
            "Failed to open PDB file");
    }

    auto tmpPdbPath = getTempPdbPath(pdbPath);
    auto tmpPdb = openFile(tmpPdbPath.c_str(), FileMode<CharT>::writeEmpty);
    if (!tmpPdb) {
        throw std::system_error(errno, std::system_category(),
            "Failed to open file");
    }

    MsfFile msf(pdb);

    msf.replaceStream(PdbStreamType::streamTable, nullptr);

    // Read the PDB header
    auto pdbHeaderStream = msf.getStream(PdbStreamType::header);

    if (!pdbHeaderStream)
        throw InvalidPdb("missing PDB header stream");

    if (pdbHeaderStream->length() < sizeof(PdbStream70))
        throw InvalidPdb("missing PDB 7.0 header");

    PdbStream70 pdbHeader;
    if (pdbHeaderStream->read(sizeof(pdbHeader), &pdbHeader) !=
            sizeof(pdbHeader))
        throw InvalidPdb("missing PDB header");

    if (pdbHeader.version < PdbVersion::vc70)
        throw InvalidPdb("unsupported PDB implementation version");

    // Check that this PDB matches what the PE file expects
    if (!pdbInfo || !matchingSignatures(*pdbInfo, pdbHeader))
        throw InvalidPdb("PE and PDB signatures do not match");

    std::cout << "PDB Timestamp: " << pdbHeader.timestamp << std::endl;
    std::cout << "PDB Age: " << pdbHeader.age << std::endl;

    msf.write(tmpPdb);

    tmpPdb.reset();
    pdb.reset();

#ifdef _WIN32
    if (dryrun) {
        // TODO
    } else {
        // TODO
    }
#else
    if (dryrun) {
        // Delete the temporary file
        if (remove(tmpPdbPath.c_str()) != 0) {
            throw std::system_error(errno, std::system_category(),
                "Failed to delete temporary PDB");
        }
    } else {
        // TODO: Rename the new PDB over top of the old one.
    }
#endif
}

template<typename CharT>
void patchImageImpl(const CharT* imagePath, const CharT* pdbPath, bool dryrun) {
    MemMap image(imagePath);

    uint8_t* buf = (uint8_t*)image.buf();
    const size_t length = image.length();

    PEFile pe = PEFile(buf, length);

    Patches patches(buf);

    patches.add(&pe.fileHeader->TimeDateStamp, &pe.timestamp,
            "IMAGE_FILE_HEADER.TimeDateStamp");

    const CV_INFO_PDB70* pdbInfo = NULL;

    switch (pe.magic()) {
        case IMAGE_NT_OPTIONAL_HDR32_MAGIC: {
            // Patch as a PE32 file
            auto opt = pe.optionalHeader<IMAGE_OPTIONAL_HEADER32>();
            pdbInfo = pe.pdbInfo(opt);
            patchOptionalHeader(pe, patches, opt);
            break;
        }

        case IMAGE_NT_OPTIONAL_HDR64_MAGIC: {
            // Patch as a PE32+ file
            auto opt = pe.optionalHeader<IMAGE_OPTIONAL_HEADER64>();
            pdbInfo = pe.pdbInfo(opt);
            patchOptionalHeader(pe, patches, opt);
            break;
        }

        default:
            throw InvalidImage("unsupported IMAGE_NT_HEADERS.OptionalHeader");
    }

    patches.sort();

    // Calculate the checksum of the PE file. Note that the checksum is stored
    // in the PDB signature. When the patches are applied, this checksum is what
    // will be set in the file.
    calculateChecksum(buf, length, patches.patches, pe.pdbSignature);

    // Patch the PDB file.
    if (pdbPath) {
        patchPDB(pdbPath, pdbInfo, pe.pdbSignature, dryrun);
    }

    patches.apply(dryrun);
}

}

void patchImage(const char* imagePath, const char* pdbPath, bool dryrun) {
    patchImageImpl(imagePath, pdbPath, dryrun);
}

#ifdef _WIN32
void patchImage(const wchar_t* imagePath, const wchar_t* pdbPath, bool dryrun) {
    patchImageImpl(imagePath, pdbPath, dryrun);
}
#endif