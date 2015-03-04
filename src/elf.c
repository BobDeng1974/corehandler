#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <elf.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

#define warn	warning
#define warnx	warning

#define TARGET_BITS 32

#if TARGET_BITS == 64
typedef Elf64_Ehdr	Elf_Ehdr;
typedef Elf64_Shdr	Elf_Shdr;
typedef Elf64_Sym	Elf_Sym;
#else
typedef Elf32_Ehdr	Elf_Ehdr;
typedef Elf32_Shdr	Elf_Shdr;
typedef Elf32_Sym	Elf_Sym;
#endif
#define ALIGN	(sizeof(unsigned long))
#define IS_ALIGNED(ptr)	(((unsigned long)(ptr) % ALIGN) == 0)

/*
 * An ELF file.
 */
struct ef {
	char		*path;
	const char	*ptr;		// Pointer to memory map of the whole file.
	size_t		 size;		// Size of memory map.
	const Elf_Ehdr	*hdr;		// Pointer to ELF header.
	unsigned long	 shnum;		// Number of section headers.
};

#define Y(x)	{ x, #x }
static const struct strtab {
	unsigned long	 key;
	const char	*val;
} classes[] = {
	Y(ELFCLASSNONE),
	Y(ELFCLASS32),
	Y(ELFCLASS64),
	{ 0, NULL },
}, dataencodings[] = {
	Y(ELFDATANONE),
	Y(ELFDATA2LSB),
	Y(ELFDATA2MSB),
	{ 0, NULL },
}, abis[] = {
	Y(ELFOSABI_NONE),
	Y(ELFOSABI_SYSV),
	Y(ELFOSABI_HPUX),
	Y(ELFOSABI_NETBSD),
	Y(ELFOSABI_GNU),
	Y(ELFOSABI_SOLARIS),
	Y(ELFOSABI_AIX),
	Y(ELFOSABI_IRIX),
	Y(ELFOSABI_FREEBSD),
	Y(ELFOSABI_TRU64),
	Y(ELFOSABI_MODESTO),
	Y(ELFOSABI_OPENBSD),
	Y(ELFOSABI_ARM_AEABI),
	Y(ELFOSABI_ARM),
	Y(ELFOSABI_STANDALONE),
	{ 0, NULL },
}, objtypes[] = {
	Y(ET_NONE),
	Y(ET_REL),
	Y(ET_EXEC),
	Y(ET_DYN),
	Y(ET_CORE),
	{ 0, NULL },
}, sectiontypes[] = {
	Y(SHT_NULL),
	Y(SHT_PROGBITS),
	Y(SHT_SYMTAB),
	Y(SHT_STRTAB),
	Y(SHT_RELA),
	Y(SHT_HASH),
	Y(SHT_DYNAMIC),
	Y(SHT_NOTE),
	Y(SHT_NOBITS),
	Y(SHT_REL),
	Y(SHT_SHLIB),
	Y(SHT_DYNSYM),
	Y(SHT_INIT_ARRAY),
	Y(SHT_FINI_ARRAY),
	Y(SHT_PREINIT_ARRAY),
	Y(SHT_GROUP),
	Y(SHT_SYMTAB_SHNDX),
	{ 0, NULL },
};
#undef Y

const char *
key2str(const struct strtab *tab, int key, const char *dflt)
{
	while (tab->val != NULL) {
		if (tab->key == key)
			return tab->val;
		++tab;
	}
	return dflt;
}

static void
elf_print_ehdr(const char *name, const Elf_Ehdr *h)
{
	const char	*tmp;

	printf("%s\n", name);
	printf(" ident: %3s\n", h->e_ident + 1);
	printf(" class: %s\n", key2str(classes, h->e_ident[EI_CLASS], "unknown"));
	printf(" data encoding: %s\n", key2str(dataencodings, h->e_ident[EI_DATA], "unknown"));
	printf(" os abi: %s\n", key2str(abis, h->e_ident[EI_OSABI], "unknown"));
	printf(" abi version: %d\n", h->e_ident[EI_ABIVERSION]);
	printf(" version: %d\n", h->e_ident[EI_VERSION]);
	tmp = key2str(objtypes, h->e_type, NULL);
	if (tmp == NULL) {
		if (h->e_type >= ET_LOOS && h->e_type <= ET_HIOS)
			tmp = "OS specific";
		else if (h->e_type >= ET_LOPROC && h->e_type <= ET_HIPROC)
			tmp = "Processor specific";
		else
			tmp = "unknown";
	}
	printf(" type: %s\n", tmp);
	printf(" machine: %d\n", h->e_machine);
	printf(" version: %d\n", h->e_version);
	printf(" entry: 0x%lx\n", (unsigned long)h->e_entry);
	if (h->e_phoff > 0)
		printf(" program header: 0x%lx\n", (unsigned long)h->e_phoff);
	if (h->e_shoff > 0)
		printf(" section header: 0x%lx\n", (unsigned long)h->e_shoff);
	if (h->e_shstrndx != SHN_UNDEF)
		printf(" section name table index: %d\n", (int)h->e_shstrndx);
}

static void
elf_print_shdr_raw(const Elf_Shdr *h)
{
	printf("Elf_Shdr:\n");
#define P(x)	printf(" " #x ": 0x%lx\n", (unsigned long)h->x)
	P(sh_name);
	P(sh_type);
	P(sh_flags);
	P(sh_flags);
	P(sh_addr);
	P(sh_offset);
	P(sh_size);
	P(sh_link);
	P(sh_info);
	P(sh_addralign);
	P(sh_entsize);
#undef P
}

// FIXME implement checks of EVERYTHING

static const Elf_Shdr *
elf_get_shdr_type(const struct ef *ef, unsigned long type)
{
	unsigned long	 i;
	const Elf_Shdr	*sh;

	for (i = 0; i < ef->shnum; ++i) {
		sh = (const Elf_Shdr *)(ef->ptr + ef->hdr->e_shoff + ef->hdr->e_shentsize * i);
		if (sh->sh_type == type)
			return sh;
	}

	return NULL;
}

/*
 * Return header of section i.
 */
static const Elf_Shdr *
elf_get_shdr_index(const struct ef *ef, unsigned long ndx)
{
	const char	*data;

	if (ndx >= ef->shnum) {
		warnx("%s: attempt to access section header %lu while there are only %lu section headers", ef->path, ndx, ef->shnum);
		return NULL;
	}

	data = ef->ptr + ef->hdr->e_shoff + ef->hdr->e_shentsize * ndx;

	if (data >= (ef->ptr + ef->size) - sizeof(Elf_Shdr)) {
		warnx("%s: invalid offset/size for section header %lu", ef->path, ndx);
		return NULL;
	}

	return (const Elf_Shdr *)data;
}

unsigned long
min_ulong(unsigned long a, unsigned long b)
{
	if (a <= b)
		return a;
	else
		return b;
}

static const Elf_Shdr *
elf_get_shdr_name(const struct ef *ef, const char *name)
{
	unsigned long	 ndx;
	const Elf_Shdr	*sh;
	const char	*strtab;
	size_t		 strtabsize;
	size_t		 cmpsize;
	size_t		 namelen;

	if (ef->hdr->e_shstrndx == SHN_UNDEF) {
		warnx("%s: file has no section name string table", ef->path);
		return NULL;
	}

	/*
	 * To search for a section by name, we need to first find the string
	 * table section which contains the names of all sections.
	 */

	/*
	 * Fetch the section name string table header's index.
	 */

	ndx = ef->hdr->e_shstrndx;
	if (ndx == SHN_XINDEX) {
		sh = elf_get_shdr_index(ef, 0);
		if (sh == NULL)
			return NULL;
		ndx = sh->sh_link;
	}
	if (ndx >= ef->hdr->e_shnum) {
		warnx("%s: invalid index for section name string table", ef->path);
		return NULL;
	}

	/*
	 * Using the index, fetch the header and using the header, fetch a
	 * pointer to string table's data.
	 */

	sh = elf_get_shdr_index(ef, ndx);
	if (sh == NULL)
		return NULL;
	strtab = ef->ptr + sh->sh_offset;
	strtabsize = sh->sh_size;
	if (strtab + strtabsize  >= ef->ptr + ef->size) {
		warnx("%s: invalid offset/size for section name string table", ef->path);
		return NULL;
	}

	/*
	 * Now that we can resolve the names of sections, walk the section
	 * headers and return a header if it's name matches what we're
	 * looking for.
	 */

	namelen = strlen(name);
	for (ndx = 0; ndx < ef->hdr->e_shnum; ++ndx) {
		sh = elf_get_shdr_index(ef, ndx);
		if (sh == NULL)
			return NULL;

		if (sh->sh_name >= strtabsize) {
			warnx("%s: invalid string table index in section header %lu", ef->path, ndx);
			return NULL;
		}

		cmpsize = min_ulong(namelen, strtabsize - sh->sh_name);
		if (strncmp(strtab + sh->sh_name, name, cmpsize) == 0)
			return sh;
	}

	return NULL;
}

static const char *
elf_get_sdata(const struct ef *ef, const Elf_Shdr *sh)
{
	const char	*data;

	if (sh == NULL)
		return NULL;

	if (sh->sh_type == SHT_NULL || sh->sh_type == SHT_NOBITS) {
		warnx("%s: attempt to access data of section which has no on-disk data (section type is %lu)", ef->path, (unsigned long)sh->sh_type);
		return NULL;
	}

	data = ef->ptr + sh->sh_offset;

	if (data >= ef->ptr + ef->size) {
		warnx("%s: invalid data offset in section header", ef->path);
		return NULL;
	}

	return data;
}

static void
elf_print_shdr(const struct ef *ef, unsigned long ndx, const Elf_Shdr *sh)
{
	const char	*tmp;
	char		 buf[64];
	const char	*strtab;

	printf("Section header %lu:\n", ndx);

	strtab = elf_get_sdata(ef, elf_get_shdr_index(ef, ef->hdr->e_shstrndx));

	printf(" name: %s\n", strtab + sh->sh_name);
	if (sh->sh_type >= SHT_LOOS) {
		if (sh->sh_type <= SHT_HIOS)
			tmp = "SHT_LOOS, SHT_HIOS";
		else if (sh->sh_type <= SHT_HIPROC)
			tmp = "SHT_LOPROC, SHT_HIPROC";
		else if (sh->sh_type <= SHT_HIUSER)
			tmp = "SHT_LOUSER, SHT_HIUSER";
		(void)snprintf(buf, sizeof buf, "%lu (%s)", (unsigned long)sh->sh_type, tmp);
		tmp = buf;
	} else {
		tmp = key2str(sectiontypes, sh->sh_type, NULL);
		if (tmp == NULL) {
			(void)snprintf(buf, sizeof buf, "%lu (?)", (unsigned long)sh->sh_type);
			tmp = buf;
		}
	}
	printf(" type: %s\n", tmp);
}

static bool
elf_is_valid(const char *ptr, size_t size, const char **errstr)
{
	Elf_Ehdr	*e;
	size_t		 n;

	if (size < sizeof *e) {
		*errstr = "ELF header goes beyond end of file";
		return false;
	}

	e = (Elf_Ehdr *)ptr;

	if (e->e_shoff > 0) {
		if (e->e_shoff >= size) {
			debug("e_shoff=%lu", (unsigned long)e->e_shoff);
			*errstr = "first section header is beyond end of file";
			return false;
		}
		if (e->e_shentsize < sizeof(Elf_Shdr)) {
			*errstr = "size of section headers is less than expected";
			return false;
		}
		if (e->e_shnum > 0) {
			n = e->e_shnum;
		} else {
			n = ((Elf_Shdr *)(ptr + e->e_shoff))->sh_size;
		}
		if (e->e_shoff + e->e_shentsize * n > size) {
			*errstr = "section header table goes beyond end of file";
			return false;
		}
	}

	return true;
}

struct ef *
elf_open(const char *path)
{
	struct stat	 st;
	int		 fd;
	const void	*ptr;
	const char	*errstr;
	struct ef	*ef;
	Elf_Shdr	*sh;

	if (stat(path, &st) == -1) {
		warn("%s: stat", path);
		return NULL;
	}
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		warn("%s: open", path);
		return NULL;
	}
	/*
	 * Regarding ELF data structure alignment:
	 * Linux mmap(2) manpage promises that the returned pointer will be
	 * memory page size aligned and ELF specification promises that ELF
	 * structures are naturally self-aligned.
	 */
	ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	(void)close(fd);
	if (ptr == MAP_FAILED) {
		warn("%s: mmap", path);
		return NULL;
	}
	debug("mmap'ed %s: ptr=%p, size=%lu", path, ptr, (unsigned long)st.st_size);

	if (!elf_is_valid(ptr, st.st_size, &errstr)) {
		warnx("%s: %s", path, errstr); // FIXME replace with debug()/warning()/fatal() ...
		(void)munmap((void *)ptr, st.st_size);
		return NULL;
	}

	ef = malloc(sizeof *ef); // FIXME replace with xmalloc
	ef->path = strdup(path); // FIXME replace with xstrdup
	ef->ptr = ptr;
	ef->size = st.st_size;
	ef->hdr = ptr;
	if (ef->hdr->e_shoff > 0) {
		if (ef->hdr->e_shnum > 0) {
			ef->shnum = ef->hdr->e_shnum;
		} else {
			sh = (Elf_Shdr *)(ef->ptr + ef->hdr->e_shoff);
			ef->shnum = sh->sh_size;
		}
	}

	return ef;
}

void
elf_close(struct ef *ef)
{
	free(ef->path);
	munmap((void *)ef->ptr, ef->size);
	free(ef);
}

/*
 * Return true if ptr1+size1 encompasses ptr2+size2.
 */
static bool
is_subrange(const char *ptr1, size_t size1, const char *ptr2, size_t size2)
{
	if (ptr2 >= ptr1 && size2 <= size1)
		return true;
	return false;
}

static void
elf_print_sym_raw(const Elf_Sym *s)
{
#define Y(x)	printf(" %s: 0x%lx\n", #x, (unsigned long)s->x)
	Y(st_name);
	Y(st_value);
	Y(st_size);
	Y(st_info);
	Y(st_other);
	Y(st_shndx);
#undef Y
}

bool
elf_is_shared_object(const struct ef *ef)
{
	return ef->hdr->e_type == ET_DYN;
}

// FIXME provide offset from beginning?
const char *
elf_resolve_sym(const struct ef *ef, unsigned long addr)
{
	const unsigned long	*type = (const unsigned long[]){
		SHT_SYMTAB,
		SHT_DYNSYM,
		0
	};
	const Elf_Shdr		*symtabhdr;
	const Elf_Shdr		*strtabhdr;
	const Elf_Shdr		*sh;
	const char		*strtab;
	unsigned long		 strtabsize;
	const char		*data;
	const Elf_Sym		*sym;

	// FIXME need to read out data, cannot just use pointers directly!
	// FIXME overflow! fix elsewhere too
	// FIXME inadequate checking (entsize!), think of a better way, elsewhere too
#if 0 // FIXME doesnt work
	if (!IS_ALIGNED(strtab) || !is_subrange(ef->ptr, ef->size, strtab, strtabsize)) {
		warnx("%s: invalid offset/size of .symtab section data", ef->path);
		return false;
	}
#endif
	
	/*
	 * For each of the symbol table types...
	 */

	while (*type != 0) {
		symtabhdr = elf_get_shdr_type(ef, *type++);
		if (symtabhdr == NULL)
			continue;
		strtabhdr = elf_get_shdr_index(ef, symtabhdr->sh_link);
		if (strtabhdr == NULL)
			continue;
		strtab = ef->ptr + strtabhdr->sh_offset; // FIXME potential overflow
		strtabsize = strtabhdr->sh_size;

		if (!is_subrange(ef->ptr, ef->size, strtab, strtabsize) ||
		    strtabsize == 0) {
			warnx("%s: invalid offset/size of symbol name string table", ef->path);
			return NULL;
		}

		if (strtab[strtabsize - 1] != '\0') {
			warnx("%s: symbol name string table is not null-terminated", ef->path);
			return NULL;
		}

		if (!is_subrange(ef->ptr, ef->size, ef->ptr + symtabhdr->sh_offset, symtabhdr->sh_size) // FIXME overflow!
		    || symtabhdr->sh_size == 0) {
			warnx("%s: invalid offset/size for symbol table", ef->path);
			return NULL;
		}
		if (symtabhdr->sh_entsize < sizeof(Elf_Sym) || symtabhdr->sh_entsize >= ef->size) {
			warnx("%s: invalid entity size for symbol table", ef->path);
			return NULL;
		}
		
		/*
		 * ...walk the symbol table and find the symbol which matches
		 * the translated address.
		 */

		data = ef->ptr + symtabhdr->sh_offset;
		for (data += symtabhdr->sh_entsize; /* Skip element 0: it's the 'Undefined Symbol'. */
		    data < (ef->ptr + symtabhdr->sh_offset + symtabhdr->sh_size);
		    data += symtabhdr->sh_entsize) {
			sym = (const Elf_Sym *)data;

			// FIXME validate sym values
			if (addr < sym->st_value || addr >= sym->st_value + sym->st_size)
				continue;

			// FIXME check that the name is null terminated OR strdup?
			if (sym->st_name > 0 && sym->st_name < strtabsize)
				return strtab + sym->st_name;
			else {
				debug("symbol found for addr %lx, but it doesn't appear to have a name...", addr);
				return NULL;
			}
		}
	}
	return NULL;
}

static void
elf_print_symtabs(const struct ef *ef)
{
	const unsigned long	*type;
	const Elf_Shdr		*sh;
	const char		*data;
	const char		*end;
	const char		*strtab;
	const Elf_Sym		*sym;

	printf("\nSymbols:\n");

	for (type = (const unsigned long[]){ SHT_SYMTAB, SHT_DYNSYM, 0 }; *type != 0; type++) {
		sh = elf_get_shdr_type(ef, *type);
		if (sh == NULL)
			continue;
		elf_print_shdr_raw(sh);

		strtab = elf_get_sdata(ef, elf_get_shdr_index(ef, sh->sh_link));
		printf("strtab=%p\n", strtab);
		printf("strtab: %s\n", strtab);

		data = elf_get_sdata(ef, sh) + sh->sh_entsize;
		end = ef->ptr + sh->sh_offset + sh->sh_size;
		while (data < end) {
			sym = (const Elf_Sym *)data;
			if (sym->st_name > 0) {
				printf("[%s]\n", strtab + sym->st_name);
			}
			elf_print_sym_raw(sym);
			printf("\n");
			data += sh->sh_entsize;
		}
	}
}

static void
elf_print_info(const char *path)
{
	const struct ef	*ef;
	unsigned long	 i;

	ef = elf_open(path);
	if (ef == NULL) {
		warnx("%s: failed to open elf file", path);
		return;
	}

	elf_print_ehdr(path, ef->hdr);

	for (i = 0; i < ef->shnum; ++i)
		elf_print_shdr(ef, i, elf_get_shdr_index(ef, i));

	elf_print_symtabs(ef);
}

