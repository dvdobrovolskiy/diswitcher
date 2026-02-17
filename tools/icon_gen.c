#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

static int DrawIconPixels(int sizePx, uint32_t** outPixels)
{
    *outPixels = NULL;

    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sizePx;
    bi.bV5Height = -sizePx; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = NULL;
    HDC hdc = GetDC(NULL);
    if (!hdc) return 0;
    HBITMAP colorBmp = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!colorBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        return 0;
    }

    const size_t pixelCount = (size_t)sizePx * (size_t)sizePx;
    ZeroMemory(bits, pixelCount * 4);

    HDC memdc = CreateCompatibleDC(NULL);
    if (!memdc) {
        DeleteObject(colorBmp);
        return 0;
    }
    HGDIOBJ oldBmp = SelectObject(memdc, colorBmp);

    const COLORREF red = RGB(220, 0, 0);
    const COLORREF white = RGB(255, 255, 255);
    const int penW = (sizePx / 10) > 2 ? (sizePx / 10) : 2;
    const int pad = (sizePx / 12) > 2 ? (sizePx / 12) : 2;

    HPEN pen = CreatePen(PS_SOLID, penW, red);
    HBRUSH brush = CreateSolidBrush(red);
    HGDIOBJ oldPen = SelectObject(memdc, pen);
    HGDIOBJ oldBrush = SelectObject(memdc, brush);

    Ellipse(memdc, pad, pad, sizePx - pad, sizePx - pad);

    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, white);

    const int fontSize = (int)(sizePx * 0.70);
    HFONT font = CreateFontW(
        -fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = NULL;
    if (font) oldFont = SelectObject(memdc, font);

    RECT rc = {0, 0, sizePx, sizePx};
    DrawTextW(memdc, L"S", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (oldFont) SelectObject(memdc, oldFont);
    if (font) DeleteObject(font);

    SelectObject(memdc, oldBrush);
    SelectObject(memdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    // Ensure alpha is set for any drawn pixel (GDI often leaves alpha at 0).
    uint32_t* px = (uint32_t*)bits;
    for (size_t i = 0; i < pixelCount; i++) {
        if ((px[i] & 0x00FFFFFFu) != 0) {
            px[i] |= 0xFF000000u;
        }
    }

    SelectObject(memdc, oldBmp);
    DeleteDC(memdc);

    uint32_t* copy = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, pixelCount * 4);
    if (!copy) {
        DeleteObject(colorBmp);
        return 0;
    }
    memcpy(copy, px, pixelCount * 4);
    DeleteObject(colorBmp);

    *outPixels = copy;
    return 1;
}

static int WriteIco(const wchar_t* path, const int* sizes, int count)
{
    // ICO with BMP/DIB images (32bpp) + empty AND mask.
    FILE* f = NULL;
    _wfopen_s(&f, path, L"wb");
    if (!f) return 0;

    // ICONDIR
    uint16_t reserved = 0;
    uint16_t type = 1;
    uint16_t n = (uint16_t)count;
    fwrite(&reserved, 2, 1, f);
    fwrite(&type, 2, 1, f);
    fwrite(&n, 2, 1, f);

    const long dirStart = ftell(f);
    // Placeholder directory entries (16 bytes each)
    uint8_t zero16[16] = {0};
    for (int i = 0; i < count; i++) fwrite(zero16, 16, 1, f);

    typedef struct {
        uint32_t bytes;
        uint32_t offset;
        int size;
    } EntryInfo;

    EntryInfo infos[8];
    if (count > (int)(sizeof(infos) / sizeof(infos[0]))) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        const int s = sizes[i];
        uint32_t* pixels = NULL;
        if (!DrawIconPixels(s, &pixels)) {
            fclose(f);
            return 0;
        }

        const int width = s;
        const int height = s;
        const uint32_t xorBytes = (uint32_t)((size_t)width * (size_t)height * 4);
        const uint32_t maskStride = (uint32_t)((((uint32_t)width + 31u) / 32u) * 4u);
        const uint32_t andBytes = maskStride * (uint32_t)height;

        BITMAPINFOHEADER bih;
        ZeroMemory(&bih, sizeof(bih));
        bih.biSize = sizeof(bih);
        bih.biWidth = width;
        bih.biHeight = height * 2; // includes AND mask
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = xorBytes + andBytes;

        const uint32_t imgBytes = (uint32_t)sizeof(bih) + xorBytes + andBytes;
        infos[i].bytes = imgBytes;
        infos[i].offset = (uint32_t)ftell(f);
        infos[i].size = s;

        fwrite(&bih, sizeof(bih), 1, f);

        // Write XOR bitmap bottom-up.
        for (int y = height - 1; y >= 0; y--) {
            const uint32_t* row = pixels + (size_t)y * (size_t)width;
            fwrite(row, 4, (size_t)width, f);
        }

        // AND mask: all 0 (opaque).
        uint8_t* maskRow = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, maskStride);
        if (!maskRow) {
            HeapFree(GetProcessHeap(), 0, pixels);
            fclose(f);
            return 0;
        }
        for (int y = 0; y < height; y++) {
            fwrite(maskRow, 1, maskStride, f);
        }
        HeapFree(GetProcessHeap(), 0, maskRow);
        HeapFree(GetProcessHeap(), 0, pixels);
    }

    // Fill directory entries.
    fseek(f, dirStart, SEEK_SET);
    for (int i = 0; i < count; i++) {
        const int s = infos[i].size;
        const uint8_t w = (s >= 256) ? 0 : (uint8_t)s;
        const uint8_t h = (s >= 256) ? 0 : (uint8_t)s;
        const uint8_t colorCount = 0;
        const uint8_t res = 0;
        const uint16_t planes = 1;
        const uint16_t bitCount = 32;
        const uint32_t bytesInRes = infos[i].bytes;
        const uint32_t offset = infos[i].offset;

        fwrite(&w, 1, 1, f);
        fwrite(&h, 1, 1, f);
        fwrite(&colorCount, 1, 1, f);
        fwrite(&res, 1, 1, f);
        fwrite(&planes, 2, 1, f);
        fwrite(&bitCount, 2, 1, f);
        fwrite(&bytesInRes, 4, 1, f);
        fwrite(&offset, 4, 1, f);
    }

    fclose(f);
    return 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: icon_gen <output.ico>\n");
        return 2;
    }
    const int sizes[] = {16, 32, 48};
    if (!WriteIco(argv[1], sizes, (int)(sizeof(sizes) / sizeof(sizes[0])))) {
        fwprintf(stderr, L"failed to write ico\n");
        return 1;
    }
    return 0;
}

