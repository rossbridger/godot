/**************************************************************************/
/*  hash_map.h                                                            */
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

#ifndef HASH_MAP_H
#define HASH_MAP_H

#include "core/math/math_funcs.h"
#include "core/os/memory.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/paged_allocator.h"
#include "core/templates/pair.h"

/**
 * A HashMap implementation that uses power-of-2 size with open addressing.
 * Keys and values are stored in a array by insertion order.
 *
 * The assignment operator copy the pairs from one map to the other.
 */

template <typename TKey, typename TValue>
struct HashMapElement {
	KeyValue<TKey, TValue> data;
	HashMapElement() {}
	HashMapElement(const TKey &p_key, const TValue &p_value) :
			data(p_key, p_value) {}
};

template <typename TKey, typename TValue,
		typename Hasher = HashMapHasherDefault,
		typename Comparator = HashMapComparatorDefault<TKey>>
class HashMap {
public:
	static constexpr uint32_t MIN_CAPACITY_INDEX = 2; // Use a prime.
	static constexpr float MAX_OCCUPANCY = 0.8;
	static constexpr uint32_t EMPTY_HASH = 0 - 1u;
	static constexpr uint32_t EAD = 2;

private:
	struct Index {
		uint32_t next;
		uint32_t slot;
	};
	HashMapElement<TKey, TValue> *elements = nullptr;
	Index *index = nullptr;

	uint32_t capacity_index = 0;
	uint32_t num_elements = 0;
	uint32_t last_pos = 0;
	uint32_t etail = EMPTY_HASH;

	_FORCE_INLINE_ uint32_t _hash(const TKey &p_key) const {
		uint32_t hash = Hasher::hash(p_key);
		return hash;
	}

	bool _lookup_pos(const TKey &p_key, uint32_t &r_pos) const {
		if (elements == nullptr || num_elements == 0) {
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
			if (((index[bucket].slot & ~mask) == (hash & ~mask)) && Comparator::compare(elements[pos].data.key, p_key)) {
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
		if ((index[++bucket].next == EMPTY_HASH) || (index[++bucket].next == EMPTY_HASH))
			return bucket;

		constexpr uint32_t linear_probe_length = 6u;
		for (uint32_t offset = 4u, step = 3u; step < linear_probe_length;) {
			bucket = (p_bucket_from + offset) & mask;
			if ((index[bucket].next == EMPTY_HASH) || (index[++bucket].next == EMPTY_HASH))
				return bucket;
			offset += step++;
		}

		while (true) {
			last_pos &= mask;
			if (index[++last_pos].next == EMPTY_HASH) {
				return last_pos;
			}
			uint32_t medium = (num_elements / 2 + last_pos) & mask;
			if (index[medium].next == EMPTY_HASH) {
				return medium;
			}
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

	// p_value.data.key must not exist in elements
	void _insert_with_hash(uint32_t p_hash, const TKey &p_key, const TValue &p_value) {
		const uint32_t mask = (1 << capacity_index) - 1;
		uint32_t hash = p_hash;
		uint32_t bucket = _find_unique_bucket(hash);
		new(&elements[num_elements]) HashMapElement<TKey, TValue>(p_key, p_value);
		etail = bucket;
		index[bucket] = { bucket, num_elements++ | (hash & mask) };
	}

	void _resize_and_rehash(uint32_t p_new_capacity_index) {
		uint32_t old_capacity = 1 << capacity_index;
		uint32_t old_mask = old_capacity - 1;

		// Capacity can't be 0.
		capacity_index = MAX((uint32_t)MIN_CAPACITY_INDEX, p_new_capacity_index);

		uint32_t capacity = 1 << capacity_index;

		HashMapElement<TKey, TValue> *old_elements = elements;
		Index *old_index = index;

		num_elements = 0;
		index = reinterpret_cast<Index *>(Memory::alloc_static(sizeof(Index) * (capacity + EAD)));
		elements = reinterpret_cast<HashMapElement<TKey, TValue> *>(Memory::alloc_static(sizeof(HashMapElement<TKey, TValue>) * capacity));

		memset(index, EMPTY_HASH, sizeof(Index) * capacity);
		memset(index + capacity, 0, sizeof(Index) * EAD);
		memset(elements, 0, sizeof(HashMapElement<TKey, TValue>) * capacity);

		if (old_capacity == 0) {
			// Nothing to do.
			return;
		}

		memcpy((char *)elements, (char *)old_elements, num_elements * sizeof(HashMapElement<TKey, TValue>));
		Memory::free_static(old_elements);
		Memory::free_static(old_index);

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

	_FORCE_INLINE_ HashMapElement<TKey, TValue> *_insert(const TKey &p_key, const TValue &p_value, bool p_front_insert = false) {
		uint32_t capacity = 1 << capacity_index;
		if (unlikely(elements == nullptr)) {
			// Allocate on demand to save memory.

			index = reinterpret_cast<Index *>(Memory::alloc_static(sizeof(Index) * (capacity + EAD)));
			elements = reinterpret_cast<HashMapElement<TKey, TValue> *>(Memory::alloc_static(sizeof(HashMapElement<TKey, TValue>) * capacity));

			memset(index, EMPTY_HASH, sizeof(Index) * capacity);
			memset(index + capacity, 0, sizeof(Index) * EAD);
			memset(elements, 0, sizeof(HashMapElement<TKey, TValue>) * capacity);
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
			_insert_with_hash(hash, p_key, p_value);
			return &elements[num_elements - 1];
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

	void _init_from(const HashMap &p_other) {
		reserve(1 << capacity_index);
		if (p_other.elements == nullptr) {
			return; // Nothing to copy.
		}

		for (const KeyValue<TKey, TValue> &E : p_other) {
			insert(E.key, E.value);
		}
	}

public:
	_FORCE_INLINE_ uint32_t get_capacity() const { return 1 << capacity_index; }
	_FORCE_INLINE_ uint32_t size() const { return num_elements; }

	/* Standard Godot Container API */

	bool is_empty() const {
		return num_elements == 0;
	}

	void clear() {
		if (elements == nullptr || num_elements == 0) {
			return;
		}
		uint32_t capacity = 1 << capacity_index;
		memset(index, EMPTY_HASH, sizeof(Index) * capacity);
		memset(elements, 0, sizeof(HashMapElement<TKey, TValue>) * capacity);
		num_elements = 0;
		last_pos = 0;
		etail = EMPTY_HASH;
	}

	TValue &get(const TKey &p_key) {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		CRASH_COND_MSG(!exists, "HashMap key not found.");
		return elements[pos].data.value;
	}

	const TValue &get(const TKey &p_key) const {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		CRASH_COND_MSG(!exists, "HashMap key not found.");
		return elements[pos].data.value;
	}

	const TValue *getptr(const TKey &p_key) const {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);

		if (exists) {
			return &elements[pos].data.value;
		}
		return nullptr;
	}

	TValue *getptr(const TKey &p_key) {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);

		if (exists) {
			return &elements[pos].data.value;
		}
		return nullptr;
	}

	_FORCE_INLINE_ bool has(const TKey &p_key) const {
		uint32_t _pos = 0;
		return _lookup_pos(p_key, _pos);
	}

	bool erase(const TKey &p_key) {
		const uint32_t mask = (1 << capacity_index) - 1;
		if (elements == nullptr || num_elements == 0) {
			return false; // Failed lookups, no elements
		}
		uint32_t hash = _hash(p_key);
		uint32_t bucket = _lookup_bucket(p_key, hash);
		if (bucket != EMPTY_HASH) {
			_erase_slot(bucket, hash & mask);
		}
		return true;
	}

	// Replace the key of an entry in-place, without invalidating iterators or changing the entries position during iteration.
	// p_old_key must exist in the map and p_new_key must not, unless it is equal to p_old_key.
	bool replace_key(const TKey &p_old_key, const TKey &p_new_key) {
		const uint32_t mask = (1 << capacity_index) - 1;
		if (p_old_key == p_new_key) {
			return true;
		}
		uint32_t pos = 0;
		ERR_FAIL_COND_V(_lookup_pos(p_new_key, pos), false);
		ERR_FAIL_COND_V(!_lookup_pos(p_old_key, pos), false);

		// delete the old index and allocate a new index, without moving elements.
		uint32_t old_hash = _hash(p_old_key);
		uint32_t old_bucket = _pos_to_bucket(pos);
		_erase_bucket(old_bucket, old_hash & (1 << capacity_index - 1));
		uint32_t hash = _hash(p_new_key);
		uint32_t new_bucket = _find_unique_bucket(hash);
		elements[pos].data.key = p_new_key;
		index[new_bucket] = { new_bucket, pos | (hash & mask) };
		etail = EMPTY_HASH;
		return true;
	}

	// Reserves space for a number of elements, useful to avoid many resizes and rehashes.
	// If adding a known (possibly large) number of elements at once, must be larger than old capacity.
	void reserve(uint32_t p_new_capacity) {
		uint32_t new_index = capacity_index;

		while ((1u << new_index) < p_new_capacity) {
			ERR_FAIL_COND_MSG(new_index + 1 == (uint32_t)HASH_TABLE_SIZE_MAX, nullptr);
			new_index++;
		}

		if (new_index == capacity_index) {
			return;
		}

		if (elements == nullptr) {
			capacity_index = new_index;
			return; // Unallocated yet.
		}
		last_pos = 0;
		_resize_and_rehash(new_index);
	}

	/** Iterator API **/

	struct ConstIterator {
		_FORCE_INLINE_ const KeyValue<TKey, TValue> &operator*() const {
			return E->data;
		}
		_FORCE_INLINE_ const KeyValue<TKey, TValue> *operator->() const { return &E->data; }
		_FORCE_INLINE_ ConstIterator &operator++() {
			if (E) {
				E++;
			}
			return *this;
		}
		_FORCE_INLINE_ ConstIterator &operator--() {
			if (E) {
				E--;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const ConstIterator &b) const { return E == b.E; }
		_FORCE_INLINE_ bool operator!=(const ConstIterator &b) const { return E != b.E; }

		_FORCE_INLINE_ explicit operator bool() const {
			return E != nullptr;
		}

		_FORCE_INLINE_ ConstIterator(const HashMapElement<TKey, TValue> *p_E) { E = p_E; }
		_FORCE_INLINE_ ConstIterator() {}
		_FORCE_INLINE_ ConstIterator(const ConstIterator &p_it) { E = p_it.E; }
		_FORCE_INLINE_ void operator=(const ConstIterator &p_it) {
			E = p_it.E;
		}

	private:
		const HashMapElement<TKey, TValue> *E = nullptr;
	};

	struct Iterator {
		_FORCE_INLINE_ KeyValue<TKey, TValue> &operator*() const {
			return E->data;
		}
		_FORCE_INLINE_ KeyValue<TKey, TValue> *operator->() const { return &E->data; }
		_FORCE_INLINE_ Iterator &operator++() {
			if (E) {
				E++;
			}
			return *this;
		}
		_FORCE_INLINE_ Iterator &operator--() {
			if (E) {
				E--;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const Iterator &b) const { return E == b.E; }
		_FORCE_INLINE_ bool operator!=(const Iterator &b) const { return E != b.E; }

		_FORCE_INLINE_ explicit operator bool() const {
			return E != nullptr;
		}

		_FORCE_INLINE_ Iterator(HashMapElement<TKey, TValue> *p_E) { E = p_E; }
		_FORCE_INLINE_ Iterator() {}
		_FORCE_INLINE_ Iterator(const Iterator &p_it) { E = p_it.E; }
		_FORCE_INLINE_ void operator=(const Iterator &p_it) {
			E = p_it.E;
		}

		operator ConstIterator() const {
			return ConstIterator(E);
		}

	private:
		HashMapElement<TKey, TValue> *E = nullptr;
	};

	_FORCE_INLINE_ Iterator begin() {
		return Iterator(elements);
	}
	_FORCE_INLINE_ Iterator end() {
		return Iterator(nullptr);
	}
	_FORCE_INLINE_ Iterator last() {
		return Iterator(&elements[num_elements - 1]);
	}

	_FORCE_INLINE_ Iterator find(const TKey &p_key) {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		if (!exists) {
			return end();
		}
		return Iterator(&elements[pos]);
	}

	_FORCE_INLINE_ void remove(const Iterator &p_iter) {
		if (p_iter) {
			erase(p_iter->key);
		}
	}

	_FORCE_INLINE_ ConstIterator begin() const {
		return ConstIterator(elements);
	}
	_FORCE_INLINE_ ConstIterator end() const {
		return ConstIterator(nullptr);
	}
	_FORCE_INLINE_ ConstIterator last() const {
		return ConstIterator(&elements[num_elements - 1]);
	}

	_FORCE_INLINE_ ConstIterator find(const TKey &p_key) const {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		if (!exists) {
			return end();
		}
		return ConstIterator(&elements[pos]);
	}

	/* Indexing */

	const TValue &operator[](const TKey &p_key) const {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		CRASH_COND(!exists);
		return elements[pos].data.value;
	}

	TValue &operator[](const TKey &p_key) {
		uint32_t pos = 0;
		bool exists = _lookup_pos(p_key, pos);
		if (!exists) {
			return _insert(p_key, TValue())->data.value;
		} else {
			return elements[pos].data.value;
		}
	}

	/* Insert */

	Iterator insert(const TKey &p_key, const TValue &p_value, bool p_front_insert = false) {
		return Iterator(_insert(p_key, p_value, p_front_insert));
	}

	/* Constructors */

	HashMap(const HashMap &p_other) {
		_init_from(p_other);
	}

	void operator=(const HashMap &p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}
		if (num_elements != 0) {
			clear();
		}

		_init_from(p_other);
	}

	HashMap(uint32_t p_initial_capacity) {
		// Capacity can't be 0.
		capacity_index = 0;
		reserve(p_initial_capacity);
	}
	HashMap() {
		capacity_index = MIN_CAPACITY_INDEX;
	}

	uint32_t debug_get_hash(uint32_t p_index) {
		if (num_elements == 0) {
			return 0;
		}
		ERR_FAIL_INDEX_V(p_index, get_capacity(), 0);
		return _hash(elements[index].data.key);
	}
	Iterator debug_get_element(uint32_t p_index) {
		if (num_elements == 0) {
			return Iterator();
		}
		ERR_FAIL_INDEX_V(p_index, get_capacity(), Iterator());
		return Iterator(&elements[p_index]);
	}

	~HashMap() {
		clear();

		if (elements != nullptr) {
			Memory::free_static(elements);
			Memory::free_static(index);
		}
	}
};

#endif // HASH_MAP_H
