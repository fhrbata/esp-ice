/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/ldgen/sinfo.c
 * @brief Parser for objdump-style sections.info files.
 */
#include "sinfo.h"
#include "ice.h"

/* Strip directory components from @p path, matching os.path.basename. */
static const char *basename_of(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static struct sinfo_archive *archive_add(struct sinfo_db *db, const char *name)
{
	ALLOC_GROW(db->archives, db->n_archives + 1, db->alloc_archives);
	struct sinfo_archive *a = &db->archives[db->n_archives++];
	memset(a, 0, sizeof(*a));
	a->name = sbuf_strdup(name);
	return a;
}

static struct sinfo_object *object_add(struct sinfo_archive *a,
				       const char *name)
{
	/* AR archives can legitimately hold two members with the same
	 * file name (two compilations of util.c into liblog.a, etc.).
	 * The linker sees both; so must we.  Merge by UNIONING their
	 * section lists under a single slot rather than overwriting --
	 * Python's EntityDB dedups to the latest which loses sections
	 * from earlier copies.  Matches "*archive:object.*(sections)"
	 * glob semantics: all members with that name contribute. */
	for (int i = 0; i < a->n_objs; i++)
		if (!strcmp(a->objs[i].name, name))
			return &a->objs[i];
	ALLOC_GROW(a->objs, a->n_objs + 1, a->alloc_objs);
	struct sinfo_object *o = &a->objs[a->n_objs++];
	memset(o, 0, sizeof(*o));
	o->name = sbuf_strdup(name);
	return o;
}

static void section_add(struct sinfo_object *o, const char *name)
{
	/* Dedup on add so union-of-two-members doesn't produce doubled
	 * sections when both copies share a common section like .text. */
	for (int i = 0; i < o->n_sections; i++)
		if (!strcmp(o->sections[i], name))
			return;
	ALLOC_GROW(o->sections, o->n_sections + 1, o->alloc_sections);
	o->sections[o->n_sections++] = sbuf_strdup(name);
}

/* Leading whitespace helper. */
static const char *skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* Line shape we care about inside an object block:
 *   "  <idx> .section_name  <size>  ..."
 * objdump prepends two spaces, the index, spaces, then a dotted name.
 * Matches Python's _RE_SECTION_NAME = r'^\s*\d+\s+(\.\S+)'.
 * Returns a pointer to the section name start, or NULL if the line
 * doesn't match.  Writes a NUL after the section name (in-place).
 */
static char *match_section_line(char *line)
{
	const char *p = skip_ws(line);
	if (!isdigit((unsigned char)*p))
		return NULL;
	while (isdigit((unsigned char)*p))
		p++;
	if (*p != ' ' && *p != '\t')
		return NULL;
	p = skip_ws(p);
	if (*p != '.')
		return NULL;
	char *name_start = (char *)p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	*(char *)p = '\0';
	return name_start;
}

/* Object-start line: "<object_name>:     file format elf32-..."
 * The name is everything before the first ':'.  Returns NULL if no ':'
 * is present.  NUL-terminates the name in-place.
 */
static char *match_object_line(char *line)
{
	if (!*line || *line == ' ' || *line == '\t')
		return NULL;
	char *colon = strchr(line, ':');
	if (!colon)
		return NULL;
	if (!strstr(colon, "file format"))
		return NULL;
	*colon = '\0';
	return line;
}

/* Archive-start line: "In archive <path>:" */
static char *match_archive_line(char *line)
{
	static const char prefix[] = "In archive ";
	if (strncmp(line, prefix, sizeof(prefix) - 1) != 0)
		return NULL;
	char *p = line + sizeof(prefix) - 1;
	size_t n = strlen(p);
	while (n && (p[n - 1] == ':' || p[n - 1] == ' ' || p[n - 1] == '\t'))
		p[--n] = '\0';
	return p;
}

void sinfo_load(struct sinfo_db *db, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read '%s'", path);

	struct sinfo_archive *cur_archive = NULL;
	struct sinfo_object *cur_object = NULL;

	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		char *s;
		if ((s = match_archive_line(line)) != NULL) {
			cur_archive = archive_add(db, basename_of(s));
			cur_object = NULL;
			continue;
		}
		if (!cur_archive)
			continue;
		if ((s = match_object_line(line)) != NULL) {
			cur_object = object_add(cur_archive, s);
			continue;
		}
		if (cur_object && (s = match_section_line(line)) != NULL)
			section_add(cur_object, s);
	}

	sbuf_release(&sb);
}

/* -------------------------- AR + ELF path -------------------------- */

/* Return 1 if @p name is an ELF object member worth indexing.  AR
 * archives can hold arbitrary files; we only care about .o / .obj. */
static int is_object_member(const char *name)
{
	size_t n = strlen(name);
	return (n >= 2 && !strcmp(name + n - 2, ".o")) ||
	       (n >= 4 && !strcmp(name + n - 4, ".obj"));
}

/* Produce the same view objdump -h does: every named section, in
 * whatever order the ELF holds them.  ldgen only cares about names.
 */
static void collect_sections_from_elf(struct sinfo_object *o, const void *buf,
				      size_t len)
{
	struct elf_sections secs;
	elf_read_sections(buf, len, &secs);
	for (int i = 0; i < secs.nr; i++) {
		if (!secs.s[i].name || !secs.s[i].name[0])
			continue;
		section_add(o, secs.s[i].name);
	}
	elf_sections_release(&secs);
}

void sinfo_load_archive(struct sinfo_db *db, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read '%s'", path);

	struct sinfo_archive *a = archive_add(db, basename_of(path));

	struct ar_reader r;
	ar_reader_init(&r, sb.buf, (size_t)sb.len);

	struct ar_member m;
	while (ar_reader_next(&r, &m)) {
		if (is_object_member(m.name)) {
			struct sinfo_object *o = object_add(a, m.name);
			collect_sections_from_elf(o, m.data, m.size);
		}
		free(m.name);
	}

	sbuf_release(&sb);
}

void sinfo_free(struct sinfo_db *db)
{
	for (int i = 0; i < db->n_archives; i++) {
		struct sinfo_archive *a = &db->archives[i];
		for (int j = 0; j < a->n_objs; j++) {
			struct sinfo_object *o = &a->objs[j];
			for (int k = 0; k < o->n_sections; k++)
				free(o->sections[k]);
			free(o->sections);
			free(o->name);
		}
		free(a->objs);
		free(a->name);
	}
	free(db->archives);
	memset(db, 0, sizeof(*db));
}
