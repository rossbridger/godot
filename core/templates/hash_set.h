/**************************************************************************/
/*  hash_set.h                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef HASH_SET_H
#define HASH_SET_H

#include "core/math/math_funcs.h"
#include "core/os/memory.h"
#include "core/templates/hash_map.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/paged_allocator.h"

/**
 * Implementation of Set using a bidi indexed hash map.
 * Use RBSet instead of this only if the following conditions are met:
 *
 * - You need to keep an iterator or const pointer to Key and you intend to add/remove elements in the meantime.
 * - Iteration order does matter (via operator<)
 *
 */

template <typename TKey,
		typename Hasher = HashMapHasherDefault,
		typename Comparator = HashMapComparatorDefault<TKey>>
class HashSet {
public:
	static constexpr uint32_t MIN_CAPACITY_INDEX = 2; // Use a prime.
	static constexpr float MAX_OCCUPANCY = 0.75;
	static constexpr uint32_t EMPTY_HASH = 0 - 1u;
	static constexpr uint32_t EAD = 2;

private:
	struct Index {
		uint32_t next;
		uint32_t slot;
	};

	TKey *keys = nullptr;
	Index *index = nullptr;

	uint32_t capacity_index = 0;
	uint32_t num_elements = 0;
	uint32_t last_pos = 0;

	_FORCE_INLINE_ uint32_t _hash(const TKey &p_key) const {
		uint32_t hash = Hasher::hash(p_key);

		if (unlikely(hash == EMPTY_HASH)) {
			hash = EMPTY_HASH + 1;
		}

		return hash;
	}

	bool _lookup_pos(const TKey &p_key, uint32_t &r_pos) const {
		if (keys == nullptr || num_elements == 0) {
			return false; // Failed lookups, no elements
		}

		uint32_t hash = _hash(p_key);
		uint32_t mask = (1 << capacity_index) - 1;
		uint32_t bucket = hash & mask;
		uint32_t next_bucket = index[bucket].next;

		if (next_bucket == EMPTY_HASH) {
			return false;
		}

		while (true) {
			uint32_t pos = index[bucket].slot & mask;
			if (((index[bucket].slot & ~mask) == (hash & ~mask)) && Comparator::compare(keys[pos], p_key)) {
				r_pos = pos;
				return true;
			}
			if (next_bucket == bucket) {
				return false;
			}
			bucket = next_bucket;
			next_bucket = index[bucket].next;
		}
		return false;
	}

	/*
	  Different probing techniques usually provide a trade-off between memory locality and avoidance of clustering.
	  Since Robin Hood hashing is relatively resilient to clustering (both primary and secondary), linear probing is the most cache friendly alternativeis typically used.

	  It's the core algorithm of this hash map with highly optimization/benchmark.
	  normaly linear probing is inefficient with high load factor, it use a new 3-way linear
	  probing strategy to search empty slot. from benchmark even the load factor > 0.9, it's more 2-3 timer fast than
	  one-way search strategy.

	  1. linear or quadratic probing a few cache line for less cache miss from input slot "bucket_from".
	  2. the first  search  slot from member variant "_last", init with 0
	  3. the second search slot from calculated pos "(_num_filled + _last) & _mask", it's like a rand value
	  */
	// key is not in this mavalue. Find a place to put it.
	uint32_t _find_empty_bucket(const uint32_t p_bucket_from) {
		const uint32_t mask = (1 << capacity_index) - 1;
		uint32_t bucket = p_bucket_from;
		if ((index[++bucket].next == EMPTY_HASH) || (index[++bucket].next == EMPTY_HASH)) {
			return bucket;
		}

		uint32_t offset = 2u;
		constexpr uint32_t linear_probe_length = 6u;
		for (; offset < linear_probe_length; offset += 2) {
			uint32_t bucket1 = (bucket + offset) & mask;
			if ((index[bucket1].next == EMPTY_HASH) || (index[++bucket1].next == EMPTY_HASH))
				return bucket1;
		}
		for (uint32_t slot = bucket + offset;; slot++) {
			uint32_t bucket1 = slot++ & _mask;
			if (unlikely(index[bucket1].next == EMPTY_HASH))
				return bucket1;

			uint32_t medium = (num_elements + _last++) & mask;
			if ((index[medium].next == EMPTY_HASH) || (index[++medium].next == EMPTY_HASH)) {
				return medium;
			}
			++_last &= mask;
		}
		return 0;
	}

	uint32_t _find_prev_bucket(const uint32_t p_main_bucket, const uint32_t p_bucket) const {
		uint32_t next_bucket = index[p_main_bucket].next;
		if (next_bucket == p_bucket)
			return p_main_bucket;

		while (true) {
			const uint32_t nbucket = index[next_bucket].next;
			if (nbucket == p_bucket)
				return next_bucket;
			next_bucket = nbucket;
		}
	}

	// kick out bucket and find empty to occpuy
	// it will break the orgin link and relink again.
	// before: main_bucket-->prev_bucket --> bucket   --> next_bucket
	// after : main_bucket-->prev_bucket --> (removed)--> new_bucket--> next_bucket
	uint32_t _kickout_bucket(const uint32_t p_kmain, const uint32_t p_bucket) {
		const uint32_t next_bucket = index[p_bucket].next;
		const uint32_t new_bucket = _find_empty_bucket(next_bucket);
		const uint32_t prev_bucket = _find_prev_bucket(p_kmain, p_bucket);

		const uint32_t last_bucket = next_bucket == p_bucket ? new_bucket : next_bucket;
		index[new_bucket] = { last_bucket, index[p_bucket].slot };

		index[prev_bucket].next = new_bucket;
		index[p_bucket].next = EMPTY_HASH;

		return p_bucket;
	}

	uint32_t _find_last_bucket(uint32_t p_main_bucket) const {
		uint32_t next_bucket = index[p_main_bucket].next;
		if (next_bucket == p_main_bucket)
			return p_main_bucket;

		while (true) {
			const uint32_t nbucket = index[next_bucket].next;
			if (nbucket == next_bucket)
				return next_bucket;
			next_bucket = nbucket;
		}
	}

	uint32_t _find_unique_bucket(uint32_t p_hash) {
		const uint32_t mask = (1 << capacity_index) - 1;
		uint32_t bucket = p_hash & mask;
		uint32_t next_bucket = index[bucket].next;
		if (next_bucket == EMPTY_HASH) {
			return bucket;
		}

		//check current bucket_key is in main bucket or not
		const uint32_t pos = index[bucket].slot & mask;
		const uint32_t kmain = _hash(elements[pos].data.key) & mask;
		if (unlikely(kmain != bucket)) {
			return _kickout_bucket(kmain, bucket);
		} else if (unlikely(next_bucket != bucket)) {
			next_bucket = _find_last_bucket(next_bucket);
		}
		return index[next_bucket].next = _find_empty_bucket(next_bucket);
	}

	uint32_t _insert_with_hash(uint32_t p_hash, TKey &p_key) {
		const uint32_t mask = (1 << capacity_index) - 1;
		uint32_t hash = p_hash;
		uint32_t bucket = _find_unique_bucket(hash);
		keys[num_elements] = p_key;
		etail = bucket;
		index[bucket] = { bucket, num_elements++ | (hash & mask) };
	}

	void _resize_and_rehash(uint32_t p_new_capacity_index) {
		uint32_t old_capacity = 1 << capacity_index;
		uint32_t old_mask = old_capacity - 1;

		// Capacity can't be 0.
		capacity_index = MAX((uint32_t)MIN_CAPACITY_INDEX, p_new_capacity_index);

		uint32_t capacity = 1 << capacity_index;

		TKey *old_keys = keys;
		Index *old_hashes = index;

		num_elements = 0;
		index = reinterpret_cast<Index *>(Memory::alloc_static(sizeof(Index) * (capacity + EAD)));
		keys = reinterpret_cast<TKey *>(Memory::alloc_static(sizeof(TKey) * capacity));

		memset(index, EMPTY_HASH, sizeof(Index) * capacity);
		memset(index + capacity, 0, sizeof(Index) * EAD);
		memset(keys, 0, sizeof(TKey) * capacity);

		if (old_capacity == 0) {
			// Nothing to do.
			return;
		}

		memcpy((char *)keys, (char *)old_keys, num_elements * sizeof(TKey));
		Memory::free_static(old_keys);
		Memory::free_static(old_hashes);

		etail = EMPTY_HASH;
		last_pos = 0;
		uint32_t mask = capacity - 1;
		for (uint32_t pos = 0; pos < num_elements; ++pos) {
			const TKey &key = elements[pos].data.key;
			const uint32_t hash = _hash(key);
			const uint32_t bucket = _find_unique_bucket(hash);
			index[bucket] = { bucket, pos | (hash & ~mask) };
		}
	}

	_FORCE_INLINE_ int32_t _insert(const TKey &p_key) {
		uint32_t capacity = 1 << capacity_index;
		if (unlikely(keys == nullptr)) {
			// Allocate on demand to save memory.

			index = reinterpret_cast<Index *>(Memory::alloc_static(sizeof(Index) * (capacity + EAD)));
			keys = reinterpret_cast<TKey *>(Memory::alloc_static(sizeof(TKey) * capacity));

			memset(index, EMPTY_HASH, sizeof(Index) * capacity);
			memset(index + capacity, 0, sizeof(Index) * EAD);
			memset(keys, 0, sizeof(TKey) * capacity);
		}

		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);

		if (exists) {
			elements[pos].data.value = p_value;
			return &elements[pos];
		} else {
			if (num_elements + 1 > MAX_OCCUPANCY * capacity) {
				ERR_FAIL_COND_V_MSG(capacity_index + 1 == HASH_TABLE_SIZE_MAX, nullptr, "Hash table maximum capacity reached, aborting insertion.");
				_resize_and_rehash(capacity_index + 1);
			}

			uint32_t hash = _hash(p_key);
			_insert_with_hash(hash, p_key);
			return &keys[num_elements - 1];
		}
	}

	uint32_t _lookup_bucket(const TKey &p_key, const uint32_t p_hash) {
		uint32_t mask = (1 << capacity_index) - 1;
		uint32_t bucket = p_hash & mask;
		uint32_t next_bucket = index[bucket].next;
		uint32_t pos = 0;

		if (next_bucket == EMPTY_HASH) {
			return EMPTY_HASH;
		}

		while (true) {
			pos = index[bucket].slot & mask;
			if (((index[bucket].slot & ~mask) == (p_hash & ~mask)) && Comparator::compare(elements[pos].data.key, p_key)) {
				return bucket;
			}
			if (next_bucket == bucket) {
				return EMPTY_HASH;
			}
			bucket = next_bucket;
			next_bucket = index[bucket].next;
		}
	}

	// Find the slot with this key, or return bucket size
	uint32_t _pos_to_bucket(const uint32_t p_pos) const {
		const uint32_t mask = (1 << capacity_index) - 1;
		const uint32_t key_hash = _hash(elements[p_pos].data.key);
		uint32_t bucket = key_hash & mask;
		while (true) {
			if (likely(p_pos == (index[bucket].slot & mask))) {
				return bucket;
			}
			bucket = index[bucket].next;
		}
		return EMPTY_HASH;
	}

	uint32_t _erase_bucket(const uint32_t p_bucket, const uint32_t p_main_bucket) {
		const uint32_t next_bucket = index[p_bucket].next;
		if (p_bucket == p_main_bucket) {
			if (p_main_bucket != next_bucket) {
				const uint32_t nbucket = index[next_bucket].next;
				index[p_main_bucket] = {
					(nbucket == next_bucket) ? p_main_bucket : nbucket,
					index[next_bucket].slot
				};
			}
			return next_bucket;
		}

		const uint32_t prev_bucket = _find_prev_bucket(p_main_bucket, p_bucket);
		index[prev_bucket].next = (p_bucket == next_bucket) ? prev_bucket : next_bucket;
		return p_bucket;
	}

	void _erase_slot(const uint32_t sbucket, const uint32_t main_bucket) {
		const uint32_t mask = (1 << capacity_index) - 1;
		const uint32_t pos = index[sbucket].slot & mask;
		const uint32_t ebucket = _erase_bucket(sbucket, main_bucket);
		const uint32_t last = --num_elements;
		if (likely(pos != last)) {
			const uint32_t last_bucket = (etail == EMPTY_HASH || ebucket == etail)
					? _pos_to_bucket(last)
					: etail;
			CRASH_COND_MSG(last_bucket == EMPTY_HASH, "HashMap data corrupted.");
			new(&elements[pos]) HashMapElement<TKey, TValue>(elements[last].data.key, elements[last].data.value);
			index[last_bucket].slot = pos | (index[last_bucket].slot & ~mask);
		}

		etail = EMPTY_HASH;
		index[ebucket] = { EMPTY_HASH, 0 };
	}

	void _init_from(const HashSet &p_other) {
		capacity_index = p_other.capacity_index;
		num_elements = p_other.num_elements;

		if (p_other.num_elements == 0) {
			return;
		}

		uint32_t capacity = 1 << capacity_index;

		index = reinterpret_cast<Index *>(Memory::alloc_static(sizeof(uint32_t) * (capacity + EAD)));
		keys = reinterpret_cast<TKey *>(Memory::alloc_static(sizeof(TKey) * capacity));

		for (uint32_t i = 0; i < num_elements; i++) {
			memnew_placement(&keys[i], TKey(p_other.keys[i]));
		}

		memcpy(index, p_other.index, (capacity + EAD) * sizeof(Index));
	}

public:
	_FORCE_INLINE_ uint32_t get_capacity() const { return 1 << capacity_index; }
	_FORCE_INLINE_ uint32_t size() const { return num_elements; }

	/* Standard Godot Container API */

	bool is_empty() const {
		return num_elements == 0;
	}

	void clear() {
		if (keys == nullptr || num_elements == 0) {
			return;
		}
		uint32_t capacity = 1 << capacity_index;
		memset((char *)index, EMPTY_HASH, sizeof(Index) * num_elements);

		for (uint32_t i = 0; i < num_elements; i++) {
			keys[i].~TKey();
	}

		num_elements = 0;
	}

	_FORCE_INLINE_ bool has(const TKey &p_key) const {
		uint32_t _pos = 0;
		return _lookup_pos(p_key, _pos);
	}

	bool erase(const TKey &p_key) {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);

		if (!exists) {
			return false;
	}

		uint32_t key_pos = pos;
		pos = key_to_hash[pos]; //make hash pos

		const uint32_t capacity = hash_table_size_primes[capacity_index];
		const uint64_t capacity_inv = hash_table_size_primes_inv[capacity_index];
		uint32_t next_pos = fastmod(pos + 1, capacity_inv, capacity);
		while (index[next_pos] != EMPTY_HASH && _get_probe_length(next_pos, index[next_pos], capacity, capacity_inv) != 0) {
			uint32_t kpos = hash_to_key[pos];
			uint32_t kpos_next = hash_to_key[next_pos];
			SWAP(key_to_hash[kpos], key_to_hash[kpos_next]);
			SWAP(index[next_pos], index[pos]);
			SWAP(hash_to_key[next_pos], hash_to_key[pos]);

			pos = next_pos;
			next_pos = fastmod(pos + 1, capacity_inv, capacity);
	}

		index[pos] = EMPTY_HASH;
		keys[key_pos].~TKey();
		num_elements--;
		if (key_pos < num_elements) {
			// Not the last key, move the last one here to keep keys lineal
			memnew_placement(&keys[key_pos], TKey(keys[num_elements]));
			keys[num_elements].~TKey();
			key_to_hash[key_pos] = key_to_hash[num_elements];
			hash_to_key[key_to_hash[num_elements]] = key_pos;
		}

		return true;
	}

	// Reserves space for a number of elements, useful to avoid many resizes and rehashes.
	// If adding a known (possibly large) number of elements at once, must be larger than old capacity.
	void reserve(uint32_t p_new_capacity) {
		uint32_t new_index = capacity_index;

		while ((1 << capacity_index) < p_new_capacity) {
			ERR_FAIL_COND_MSG(new_index + 1 == (uint32_t)HASH_TABLE_SIZE_MAX, nullptr);
			new_index++;
		}

		if (new_index == capacity_index) {
			return;
		}

		if (keys == nullptr) {
			capacity_index = new_index;
			return; // Unallocated yet.
		}
		_resize_and_rehash(new_index);
	}

	/** Iterator API **/

	struct Iterator {
		_FORCE_INLINE_ const TKey &operator*() const {
			return keys[index];
		}
		_FORCE_INLINE_ const TKey *operator->() const {
			return &keys[index];
		}
		_FORCE_INLINE_ Iterator &operator++() {
			index++;
			if (index >= (int32_t)num_keys) {
				index = -1;
				keys = nullptr;
				num_keys = 0;
			}
			return *this;
		}
		_FORCE_INLINE_ Iterator &operator--() {
			index--;
			if (index < 0) {
				index = -1;
				keys = nullptr;
				num_keys = 0;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const Iterator &b) const { return keys == b.keys && index == b.index; }
		_FORCE_INLINE_ bool operator!=(const Iterator &b) const { return keys != b.keys || index != b.index; }

		_FORCE_INLINE_ explicit operator bool() const {
			return keys != nullptr;
		}

		_FORCE_INLINE_ Iterator(const TKey *p_keys, uint32_t p_num_keys, int32_t p_index = -1) {
			keys = p_keys;
			num_keys = p_num_keys;
			index = p_index;
		}
		_FORCE_INLINE_ Iterator() {}
		_FORCE_INLINE_ Iterator(const Iterator &p_it) {
			keys = p_it.keys;
			num_keys = p_it.num_keys;
			index = p_it.index;
		}
		_FORCE_INLINE_ void operator=(const Iterator &p_it) {
			keys = p_it.keys;
			num_keys = p_it.num_keys;
			index = p_it.index;
		}

	private:
		const TKey *keys = nullptr;
		uint32_t num_keys = 0;
		int32_t index = -1;
	};

	_FORCE_INLINE_ Iterator begin() const {
		return num_elements ? Iterator(keys, num_elements, 0) : Iterator();
	}
	_FORCE_INLINE_ Iterator end() const {
		return Iterator();
	}
	_FORCE_INLINE_ Iterator last() const {
		if (num_elements == 0) {
			return Iterator();
		}
		return Iterator(keys, num_elements, num_elements - 1);
	}

	_FORCE_INLINE_ Iterator find(const TKey &p_key) const {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		if (!exists) {
			return end();
		}
		return Iterator(keys, num_elements, pos);
	}

	_FORCE_INLINE_ void remove(const Iterator &p_iter) {
		if (p_iter) {
			erase(*p_iter);
		}
	}

	/* Insert */

	Iterator insert(const TKey &p_key) {
		uint32_t pos = _insert(p_key);
		return Iterator(keys, num_elements, pos);
	}

	/* Constructors */

	HashSet(const HashSet &p_other) {
		_init_from(p_other);
	}

	void operator=(const HashSet &p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}
		if (num_elements != 0) {
			clear();
		}

		if (keys != nullptr) {
			Memory::free_static(keys);
			Memory::free_static(index);
			keys = nullptr;
			index = nullptr;
		}

		_init_from(p_other);
	}

	HashSet(uint32_t p_initial_capacity) {
		// Capacity can't be 0.
		capacity_index = 0;
		reserve(p_initial_capacity);
	}
	HashSet() {
		capacity_index = MIN_CAPACITY_INDEX;
	}

	void reset() {
		clear();

		if (keys != nullptr) {
			Memory::free_static(keys);
			Memory::free_static(index);
			keys = nullptr;
			index = nullptr;
		}
		capacity_index = MIN_CAPACITY_INDEX;
	}

	~HashSet() {
		clear();

		if (keys != nullptr) {
			Memory::free_static(keys);
			Memory::free_static(index);
		}
	}
};

#endif // HASH_SET_H
