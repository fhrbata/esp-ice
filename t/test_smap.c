/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for smap.c -- the string-keyed hash map.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	/* SMAP_INIT yields a valid empty map. */
	{
		struct smap m = SMAP_INIT;
		tap_check(m.entries == NULL);
		tap_check(m.nr == 0);
		tap_check(m.alloc == 0);
		tap_check(smap_get(&m, "anything") == NULL);
		smap_release(&m);
		tap_done("SMAP_INIT yields a valid empty map");
	}

	/* smap_put inserts; smap_get returns the stored value. */
	{
		struct smap m = SMAP_INIT;
		int a = 1, b = 2, c = 3;
		tap_check(smap_put(&m, "a", &a) == NULL);
		tap_check(smap_put(&m, "b", &b) == NULL);
		tap_check(smap_put(&m, "c", &c) == NULL);
		tap_check(m.nr == 3);
		tap_check(smap_get(&m, "a") == &a);
		tap_check(smap_get(&m, "b") == &b);
		tap_check(smap_get(&m, "c") == &c);
		tap_check(smap_get(&m, "missing") == NULL);
		smap_release(&m);
		tap_done("smap_put/smap_get round-trip distinct keys");
	}

	/* Keys are copied -- mutating the source after put does not
	 * affect lookups. */
	{
		struct smap m = SMAP_INIT;
		int v = 42;
		char key[] = "original";
		smap_put(&m, key, &v);
		key[0] = 'X';
		tap_check(smap_get(&m, "original") == &v);
		tap_check(smap_get(&m, "Xriginal") == NULL);
		smap_release(&m);
		tap_done("smap_put copies the key, not the pointer");
	}

	/* Updating an existing key returns the previous value. */
	{
		struct smap m = SMAP_INIT;
		int a = 1, b = 2;
		smap_put(&m, "k", &a);
		tap_check(smap_put(&m, "k", &b) == &a);
		tap_check(smap_get(&m, "k") == &b);
		tap_check(m.nr == 1);
		smap_release(&m);
		tap_done("smap_put update returns previous value");
	}

	/* smap_remove frees the key copy and returns the value; the
	 * probe chain is rehashed so later collisions still resolve. */
	{
		struct smap m = SMAP_INIT;
		int a = 1, b = 2, c = 3;
		smap_put(&m, "a", &a);
		smap_put(&m, "b", &b);
		smap_put(&m, "c", &c);
		tap_check(smap_remove(&m, "b") == &b);
		tap_check(m.nr == 2);
		tap_check(smap_get(&m, "a") == &a);
		tap_check(smap_get(&m, "b") == NULL);
		tap_check(smap_get(&m, "c") == &c);
		tap_check(smap_remove(&m, "b") == NULL);
		smap_release(&m);
		tap_done("smap_remove returns value and rehashes chain");
	}

	/* Growth: inserting past the 70%-load threshold resizes the
	 * table while preserving all existing bindings. */
	{
		struct smap m = SMAP_INIT;
		int slots[200];
		char key[32];
		for (int i = 0; i < 200; i++) {
			slots[i] = i;
			snprintf(key, sizeof(key), "key_%d", i);
			smap_put(&m, key, &slots[i]);
		}
		tap_check(m.nr == 200);
		tap_check(m.alloc >= 256); /* 200 / 0.7 => at least 286 */

		for (int i = 0; i < 200; i++) {
			snprintf(key, sizeof(key), "key_%d", i);
			int *v = smap_get(&m, key);
			tap_check(v != NULL && *v == i);
		}
		smap_release(&m);
		tap_done(
		    "smap grows past load threshold and preserves bindings");
	}

	/* smap_iter visits every bound key exactly once. */
	{
		struct smap m = SMAP_INIT;
		int x = 0;
		smap_put(&m, "one", &x);
		smap_put(&m, "two", &x);
		smap_put(&m, "three", &x);

		int saw_one = 0, saw_two = 0, saw_three = 0, total = 0;
		const char *key;
		void *val;
		size_t it = 0;
		while (smap_iter(&m, &it, &key, &val)) {
			if (strcmp(key, "one") == 0)
				saw_one++;
			else if (strcmp(key, "two") == 0)
				saw_two++;
			else if (strcmp(key, "three") == 0)
				saw_three++;
			total++;
			tap_check(val == &x);
		}
		tap_check(total == 3);
		tap_check(saw_one == 1 && saw_two == 1 && saw_three == 1);
		smap_release(&m);
		tap_done("smap_iter visits every binding exactly once");
	}

	/* smap_release returns to the empty sentinel. */
	{
		struct smap m = SMAP_INIT;
		int v = 0;
		smap_put(&m, "whatever", &v);
		smap_release(&m);
		tap_check(m.nr == 0);
		tap_check(m.alloc == 0);
		tap_check(m.entries == NULL);
		tap_done("smap_release returns to empty sentinel");
	}

	return tap_result();
}
