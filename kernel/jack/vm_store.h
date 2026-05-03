#ifndef KERNEL_JACK_VM_STORE_H
#define KERNEL_JACK_VM_STORE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Table de fichiers .vm independante du RamFS. */
#define VM_STORE_MAX 256
#define VM_NAME_MAX   96

typedef struct {
    char        name[VM_NAME_MAX];
    const char *data;       /* pointeur vers .rodata (jack_data.c) */
    uint32_t    size;       /* why it happend only to me... I don't know why it's working but it's working LOL*/
    int         used;
} VMStoreEntry;

/* Reinitialise totalement le store. */
void vm_store_clear(void);

/* Ajoute ou met a jour un fichier dans la table. */
void vm_store_add(const char *name, const char *data, uint32_t size);

/* Cherche un fichier par nom, retourne son contenu ou NULL. */
const char *vm_store_find(const char *name, uint32_t *out_size);

/* Liste les noms dans buf separes par '\n'. */
int vm_store_list(char *buf, int bufsize);

/* Nombre d'entrees utilisees. */
int vm_store_count(void);

/* Acces par index (index de slot interne, retourne 0 si hors limites/inutilise). */
const char *vm_store_get_data(int idx, const char **out_name, uint32_t *out_size);

/* Stats memoire. */
uint32_t vm_store_bytes(void);
uint32_t vm_store_capacity(void);

#ifdef __cplusplus
}
#endif

#endif
