/*
 * dxbc_disasm — standalone helper for Task 4 shader-level RE.
 * Reads a DXBC blob (as dumped by shaderdump.c) and prints its
 * disassembly via d3dcompiler_47.dll's D3DDisassemble.
 *
 * Usage: dxbc_disasm <file.dxbc> [more.dxbc ...]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ID3D10Blob ID3D10Blob;
typedef HRESULT(WINAPI *D3DDisassemble_t)(const void *, SIZE_T, UINT, const char *, ID3D10Blob **);

/* ID3D10Blob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static void *blob_ptr(ID3D10Blob *b) {
    void **vtbl = *(void ***)b;
    return ((void *(WINAPI *)(ID3D10Blob *))vtbl[3])(b);
}
static SIZE_T blob_size(ID3D10Blob *b) {
    void **vtbl = *(void ***)b;
    return ((SIZE_T(WINAPI *)(ID3D10Blob *))vtbl[4])(b);
}
static void blob_release(ID3D10Blob *b) {
    void **vtbl = *(void ***)b;
    ((ULONG(WINAPI *)(ID3D10Blob *))vtbl[2])(b);
}

int main(int argc, char **argv) {
    HMODULE dc;
    D3DDisassemble_t disasm;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.dxbc> [...]\n", argv[0]);
        return 2;
    }

    dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) {
        fprintf(stderr, "cannot load d3dcompiler_47.dll (gle=%lu)\n", GetLastError());
        return 1;
    }
    disasm = (D3DDisassemble_t)GetProcAddress(dc, "D3DDisassemble");
    if (!disasm) {
        fprintf(stderr, "D3DDisassemble not found\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        long len;
        void *data;
        ID3D10Blob *out = NULL;
        HRESULT hr;

        if (!fp) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            continue;
        }
        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc((size_t)len);
        if (!data || fread(data, 1, (size_t)len, fp) != (size_t)len) {
            fprintf(stderr, "read failed for %s\n", argv[i]);
            fclose(fp);
            free(data);
            continue;
        }
        fclose(fp);

        printf("======== %s (%ld bytes) ========\n", argv[i], len);
        hr = disasm(data, (SIZE_T)len, 0, NULL, &out);
        if (FAILED(hr) || !out) {
            fprintf(stderr, "D3DDisassemble failed hr=0x%08lX for %s\n", (unsigned long)hr, argv[i]);
            free(data);
            continue;
        }
        fwrite(blob_ptr(out), 1, blob_size(out) - 1, stdout); /* -1: trailing NUL */
        printf("\n");
        blob_release(out);
        free(data);
    }
    return 0;
}
