/* kernel/jack/vm_store.c - Table de fichiers .vm independante */
#include "vm_store.h"
#include "../lib/string.h"

static VMStoreEntry store[VM_STORE_MAX];
static int          nentries = 0;      /* borne haute des slots utilises */
static int          used_entries = 0;  /* nombre d'entrees actives */
static uint32_t     used_bytes = 0;

static int find_slot_by_name(const char *name) {
    for (int i = 0; i < nentries; i++) {
        if (store[i].used && kstrcmp(store[i].name, name) == 0) return i;
    }
    return -1;
}

void vm_store_clear(void) {
    kmemset(store, 0, sizeof(store));
    nentries = 0;
    used_entries = 0;
    used_bytes = 0;
}

void vm_store_add(const char *name, const char *data, uint32_t size) {
    if (!name || !*name || !data) return;

    /* Mise a jour si deja present */
    int idx = find_slot_by_name(name);
    if (idx >= 0) {
        used_bytes -= store[idx].size;
        store[idx].data = data;
        store[idx].size = size;
        used_bytes += size;
        return;
    }

    /* Reutiliser un trou */
    for (int i = 0; i < nentries; i++) {
        if (!store[i].used) {
            kmemset(store[i].name, 0, VM_NAME_MAX);
            kstrncpy(store[i].name, name, VM_NAME_MAX - 1);
            store[i].name[VM_NAME_MAX - 1] = '\0';
            store[i].data = data;
            store[i].size = size;
            store[i].used = 1;
            used_entries++;
            used_bytes += size;
            return;
        }
    }

    /* Ajouter en fin si capacite dispo */
    if (nentries >= VM_STORE_MAX) return;

    kmemset(store[nentries].name, 0, VM_NAME_MAX);
    kstrncpy(store[nentries].name, name, VM_NAME_MAX - 1);
    store[nentries].name[VM_NAME_MAX - 1] = '\0';
    store[nentries].data = data;
    store[nentries].size = size;
    store[nentries].used = 1;
    nentries++;
    used_entries++;
    used_bytes += size;
}

const char *vm_store_find(const char *name, uint32_t *out_size) {
    int idx = find_slot_by_name(name);
    if (idx < 0) return 0;
    if (out_size) *out_size = store[idx].size;
    return store[idx].data;
}

int vm_store_list(char *buf, int bufsize) {
    if (!buf || bufsize <= 0) return 0;

    int n = 0;
    for (int i = 0; i < nentries; i++) {
        if (!store[i].used) continue;
        int l = (int)kstrlen(store[i].name);
        if (n + l + 2 >= bufsize) break;
        kmemcpy(buf + n, store[i].name, (uint32_t)l);
        n += l;
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return n;
}

int vm_store_count(void) { return used_entries; }

const char *vm_store_get_data(int idx, const char **out_name, uint32_t *out_size) {
    if (idx < 0 || idx >= nentries || !store[idx].used) return 0;
    if (out_name) *out_name = store[idx].name;
    if (out_size) *out_size = store[idx].size;
    return store[idx].data;
}

uint32_t vm_store_bytes(void)    { return used_bytes; }
uint32_t vm_store_capacity(void) { return VM_STORE_MAX; }

