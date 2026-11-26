/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * B-tree node layout and search.
 * Source: Apple File System Reference, revision 2020-06-22, chapter
 * "B-Trees": "btree_node_phys_t", "btree_info_t", "kvloc_t", "kvoff_t",
 * "B-Tree Flags", "B-Tree Node Flags", "btn_index_node_val_t".
 *
 * Layout used here (from the reference):
 * - btn_data starts after the 56-byte node header;
 * - the table of contents is at btn_data + btn_table_space.off, length
 *   btn_table_space.len;
 * - the key area starts right after the table of contents; key offsets
 *   count forward from its start;
 * - the value area ends at the end of the node, or before btree_info_t in
 *   a root node; value offsets count backward from that end.
 */


#include "BTree.h"

#include "Container.h"
#include "ObjectMap.h"


static const uint32 kMaxTreeDepth = 16;


BTreeNode::BTreeNode()
	:
	fBlock(NULL),
	fBlockSize(0),
	fTableStart(0),
	fKeyStart(0),
	fValueEnd(0)
{
	memset(&fFixed, 0, sizeof(fFixed));
}


BTreeNode::~BTreeNode()
{
	free(fBlock);
}


status_t
BTreeNode::Init(uint32 blockSize)
{
	fBlock = (uint8*)malloc(blockSize);
	if (fBlock == NULL)
		return B_NO_MEMORY;
	fBlockSize = blockSize;
	return B_OK;
}


const apfs_btree_info*
BTreeNode::Info() const
{
	if (!Header()->IsRoot())
		return NULL;
	return (const apfs_btree_info*)(fBlock + fBlockSize
		- sizeof(apfs_btree_info));
}


status_t
BTreeNode::Validate(const apfs_btree_info_fixed* info)
{
	const apfs_btree_node_phys* header = Header();

	uint32 type = header->o.Type();
	if (header->IsRoot()) {
		if (type != APFS_OBJECT_TYPE_BTREE)
			return B_BAD_DATA;
		memcpy(&fFixed, &Info()->fixed, sizeof(fFixed));
	} else {
		if (type != APFS_OBJECT_TYPE_BTREE_NODE || info == NULL)
			return B_BAD_DATA;
		memcpy(&fFixed, info, sizeof(fFixed));
	}

	if (fFixed.NodeSize() != fBlockSize) {
		ERROR("B-tree node size %" B_PRIu32 " is not the block size\n",
			fFixed.NodeSize());
		return B_NOT_SUPPORTED;
	}

	uint32 dataStart = sizeof(apfs_btree_node_phys);
	fValueEnd = fBlockSize - (header->IsRoot() ? sizeof(apfs_btree_info) : 0);
	fTableStart = dataStart + header->table_space.Offset();
	fKeyStart = fTableStart + header->table_space.Length();

	uint32 entrySize = header->IsFixedSize()
		? sizeof(apfs_kvoff) : sizeof(apfs_kvloc);
	if (fKeyStart > fValueEnd
		|| (uint64)header->KeyCount() * entrySize
			> header->table_space.Length()
		|| header->Level() >= kMaxTreeDepth
		|| (header->IsLeaf() != (header->Level() == 0)))
		return B_BAD_DATA;

	if (header->IsFixedSize()
		&& (fFixed.KeySize() == 0 || (header->IsLeaf()
			&& fFixed.ValueSize() == 0)))
		return B_BAD_DATA;

	return B_OK;
}


status_t
BTreeNode::KeyAt(uint32 index, const uint8** _key, uint16* _length) const
{
	if (index >= CountKeys())
		return B_BAD_INDEX;

	uint32 offset;
	uint32 length;
	if (Header()->IsFixedSize()) {
		const apfs_kvoff* entry = (const apfs_kvoff*)(fBlock + fTableStart)
			+ index;
		offset = entry->KeyOffset();
		length = fFixed.KeySize();
	} else {
		const apfs_kvloc* entry = (const apfs_kvloc*)(fBlock + fTableStart)
			+ index;
		offset = entry->k.Offset();
		length = entry->k.Length();
	}

	if (fKeyStart + offset + length > fValueEnd)
		return B_BAD_DATA;

	*_key = fBlock + fKeyStart + offset;
	*_length = length;
	return B_OK;
}


status_t
BTreeNode::ValueAt(uint32 index, const uint8** _value, uint16* _length) const
{
	if (index >= CountKeys())
		return B_BAD_INDEX;

	uint32 offset;
	uint32 length;
	if (Header()->IsFixedSize()) {
		const apfs_kvoff* entry = (const apfs_kvoff*)(fBlock + fTableStart)
			+ index;
		offset = entry->ValueOffset();
		if (IsLeaf()) {
			length = fFixed.ValueSize();
		} else {
			length = (fFixed.Flags() & APFS_BTREE_HASHED) != 0
				? sizeof(apfs_oid_t) + 64 : sizeof(apfs_oid_t);
		}
	} else {
		const apfs_kvloc* entry = (const apfs_kvloc*)(fBlock + fTableStart)
			+ index;
		offset = entry->v.Offset();
		length = entry->v.Length();
	}

	if (offset == APFS_BTOFF_INVALID)
		return B_ENTRY_NOT_FOUND;
	if (offset > fValueEnd || offset < length
		|| fValueEnd - offset < fKeyStart)
		return B_BAD_DATA;

	*_value = fBlock + fValueEnd - offset;
	*_length = length;
	return B_OK;
}


status_t
BTreeNode::ChildAt(uint32 index, apfs_oid_t* _child) const
{
	const uint8* value;
	uint16 length;
	status_t status = ValueAt(index, &value, &length);
	if (status != B_OK)
		return status;
	if (length < sizeof(apfs_oid_t))
		return B_BAD_DATA;

	// Plain oid_t, or btn_index_node_val_t whose first field is the oid.
	uint64 child;
	memcpy(&child, value, sizeof(child));
	*_child = B_LENDIAN_TO_HOST_INT64(child);
	return B_OK;
}


// #pragma mark - BTree


BTree::BTree(Container* container, ObjectMap* objectMap, apfs_xid_t xid)
	:
	fContainer(container),
	fObjectMap(objectMap),
	fTransactionID(xid),
	fRoot(APFS_OID_INVALID),
	fRootStorage(APFS_OBJ_PHYSICAL),
	fSubtype(0)
{
	memset(&fInfo, 0, sizeof(fInfo));
}


BTree::~BTree()
{
}


status_t
BTree::SetTo(apfs_oid_t root, uint32 rootType, uint32 expectedSubtype)
{
	fRoot = root;
	fRootStorage = rootType & APFS_OBJ_STORAGETYPE_MASK;
	fSubtype = expectedSubtype;

	BTreeNode node;
	status_t status = node.Init(fContainer->BlockSize());
	if (status != B_OK)
		return status;

	status = _ReadNode(fRoot, fRootStorage, node);
	if (status != B_OK)
		return status;

	if (!node.Header()->IsRoot()) {
		ERROR("B-tree root %" B_PRIu64 " is not a root node\n", root);
		return B_BAD_DATA;
	}
	status = node.Validate(NULL);
	if (status != B_OK)
		return status;

	memcpy(&fInfo, node.Info(), sizeof(fInfo));
	return B_OK;
}


status_t
BTree::_ReadNode(apfs_oid_t oid, uint32 storage, BTreeNode& node)
{
	apfs_paddr_t address;
	status_t status;

	switch (storage) {
		case APFS_OBJ_PHYSICAL:
			address = (apfs_paddr_t)oid;
			break;

		case APFS_OBJ_EPHEMERAL:
			status = fContainer->ResolveEphemeral(oid, &address);
			if (status != B_OK)
				return status;
			break;

		case APFS_OBJ_VIRTUAL:
		{
			if (fObjectMap == NULL)
				return B_BAD_DATA;
			apfs_omap_val value;
			status = fObjectMap->Lookup(oid, fTransactionID, &value);
			if (status != B_OK)
				return status;
			if ((value.Flags() & APFS_OMAP_VAL_ENCRYPTED) != 0)
				return B_NOT_SUPPORTED;
			address = value.Address();
			break;
		}

		default:
			return B_BAD_DATA;
	}

	status = fContainer->ReadObject(address, node.Buffer(), 0);
	if (status != B_OK)
		return status;

	const apfs_obj_phys* header = (const apfs_obj_phys*)node.Buffer();
	if (header->ObjectID() != oid
		|| (fSubtype != 0 && header->Subtype() != fSubtype)) {
		ERROR("B-tree node %" B_PRIu64 " (block %" B_PRId64 ") has id %"
			B_PRIu64 ", subtype %#" B_PRIx32 "\n", oid, address,
			header->ObjectID(), header->Subtype());
		return B_BAD_DATA;
	}
	return B_OK;
}


uint32
BTree::_ChildStorage() const
{
	uint32 flags = fInfo.fixed.Flags();
	if ((flags & APFS_BTREE_PHYSICAL) != 0)
		return APFS_OBJ_PHYSICAL;
	if ((flags & APFS_BTREE_EPHEMERAL) != 0)
		return APFS_OBJ_EPHEMERAL;
	return APFS_OBJ_VIRTUAL;
}


status_t
BTree::FindLessOrEqual(const void* target, apfs_key_compare compare,
	void* context, void* key, uint16* _keyLength, void* value,
	uint16* _valueLength)
{
	return _Find(target, compare, context, false, key, _keyLength, value,
		_valueLength);
}


status_t
BTree::FindExact(const void* target, apfs_key_compare compare, void* context,
	void* value, uint16* _valueLength)
{
	return _Find(target, compare, context, true, NULL, NULL, value,
		_valueLength);
}


status_t
BTree::_Find(const void* target, apfs_key_compare compare, void* context,
	bool exact, void* key, uint16* _keyLength, void* value,
	uint16* _valueLength)
{
	if (fRoot == APFS_OID_INVALID)
		return B_NO_INIT;

	BTreeNode node;
	status_t status = node.Init(fContainer->BlockSize());
	if (status != B_OK)
		return status;

	// Children of the root use the tree's own link type.
	uint32 childStorage = _ChildStorage();

	apfs_oid_t oid = fRoot;
	uint32 storage = fRootStorage;
	int32 expectedLevel = -1;

	for (uint32 depth = 0; depth < kMaxTreeDepth; depth++) {
		status = _ReadNode(oid, storage, node);
		if (status != B_OK)
			return status;
		status = node.Validate(&fInfo.fixed);
		if (status != B_OK) {
			ERROR("damaged B-tree node %" B_PRIu64 "\n", oid);
			return status;
		}
		if (expectedLevel >= 0 && node.Level() != expectedLevel) {
			ERROR("B-tree node %" B_PRIu64 " has level %u, expected %"
				B_PRId32 "\n", oid, node.Level(), expectedLevel);
			return B_BAD_DATA;
		}

		// Binary search for the last key <= target.
		int32 low = 0;
		int32 high = (int32)node.CountKeys() - 1;
		int32 found = -1;
		while (low <= high) {
			int32 middle = (low + high) / 2;
			const uint8* entryKey;
			uint16 entryLength;
			status = node.KeyAt(middle, &entryKey, &entryLength);
			if (status != B_OK)
				return status;

			int result = compare(entryKey, entryLength, target, context);
			if (result <= 0) {
				found = middle;
				low = middle + 1;
			} else
				high = middle - 1;
		}

		if (found < 0)
			return B_ENTRY_NOT_FOUND;

		if (node.IsLeaf()) {
			const uint8* entryKey;
			uint16 keyLength;
			status = node.KeyAt(found, &entryKey, &keyLength);
			if (status != B_OK)
				return status;
			if (exact && compare(entryKey, keyLength, target, context) != 0)
				return B_ENTRY_NOT_FOUND;
			if (key != NULL) {
				if (keyLength > *_keyLength)
					return B_BUFFER_OVERFLOW;
				memcpy(key, entryKey, keyLength);
				*_keyLength = keyLength;
			}

			if (value != NULL) {
				const uint8* entryValue;
				uint16 valueLength;
				status = node.ValueAt(found, &entryValue, &valueLength);
				if (status != B_OK)
					return status;
				if (valueLength > *_valueLength)
					return B_BUFFER_OVERFLOW;
				memcpy(value, entryValue, valueLength);
				*_valueLength = valueLength;
			}
			return B_OK;
		}

		status = node.ChildAt(found, &oid);
		if (status != B_OK)
			return status;
		storage = childStorage;
		expectedLevel = node.Level() - 1;
	}

	ERROR("B-tree %" B_PRIu64 " is too deep\n", fRoot);
	return B_BAD_DATA;
}


// #pragma mark - BTreeIterator


BTreeIterator::BTreeIterator(BTree* tree)
	:
	fTree(tree),
	fLeafDepth(-1),
	fAtEnd(true)
{
	fInitStatus = fLeaf.Init(tree->fContainer->BlockSize());
	if (fInitStatus == B_OK)
		fInitStatus = fInner.Init(tree->fContainer->BlockSize());
}


BTreeIterator::~BTreeIterator()
{
}


status_t
BTreeIterator::_ReadLevel(int32 depth, apfs_oid_t oid, BTreeNode& node)
{
	uint32 storage = depth == 0 ? fTree->fRootStorage : fTree->_ChildStorage();
	status_t status = fTree->_ReadNode(oid, storage, node);
	if (status != B_OK)
		return status;
	status = node.Validate(&fTree->fInfo.fixed);
	if (status != B_OK) {
		ERROR("damaged B-tree node %" B_PRIu64 "\n", oid);
		return status;
	}
	return B_OK;
}


status_t
BTreeIterator::Seek(const void* target, apfs_key_compare compare,
	void* context)
{
	if (fInitStatus != B_OK)
		return fInitStatus;
	if (fTree->fRoot == APFS_OID_INVALID)
		return B_NO_INIT;

	fAtEnd = true;
	apfs_oid_t oid = fTree->fRoot;
	int32 parentLevel = -1;

	for (int32 depth = 0; depth < (int32)kMaxDepth; depth++) {
		BTreeNode& node = fInner;
		status_t status = _ReadLevel(depth, oid, node);
		if (status != B_OK)
			return status;
		if (parentLevel >= 0 && node.Level() != parentLevel - 1)
			return B_BAD_DATA;

		uint32 count = node.CountKeys();
		if (node.IsLeaf()) {
			// First key >= target.
			int32 low = 0;
			int32 high = (int32)count - 1;
			uint32 found = count;
			while (low <= high) {
				int32 middle = (low + high) / 2;
				const uint8* key;
				uint16 keyLength;
				status = node.KeyAt(middle, &key, &keyLength);
				if (status != B_OK)
					return status;
				if (compare(key, keyLength, target, context) >= 0) {
					found = middle;
					high = middle - 1;
				} else
					low = middle + 1;
			}

			memcpy(fLeaf.Buffer(), node.Buffer(),
				fTree->fContainer->BlockSize());
			status = fLeaf.Validate(&fTree->fInfo.fixed);
			if (status != B_OK)
				return status;
			fPath[depth].oid = oid;
			fPath[depth].index = found;
			fPath[depth].count = count;
			fLeafDepth = depth;
			fAtEnd = false;
			return B_OK;
		}

		// Last key <= target, or the first child if all keys are larger.
		int32 low = 0;
		int32 high = (int32)count - 1;
		int32 found = 0;
		while (low <= high) {
			int32 middle = (low + high) / 2;
			const uint8* key;
			uint16 keyLength;
			status = node.KeyAt(middle, &key, &keyLength);
			if (status != B_OK)
				return status;
			if (compare(key, keyLength, target, context) <= 0) {
				found = middle;
				low = middle + 1;
			} else
				high = middle - 1;
		}

		if (count == 0)
			return B_BAD_DATA;
		fPath[depth].oid = oid;
		fPath[depth].index = found;
		fPath[depth].count = count;
		parentLevel = node.Level();
		status = node.ChildAt(found, &oid);
		if (status != B_OK)
			return status;
	}

	return B_BAD_DATA;
}


status_t
BTreeIterator::_DescendLeftmost(int32 depth, apfs_oid_t child,
	int32 expectedLevel)
{
	// Read from depth (whose node is child) down to a leaf, always taking
	// the first entry. Each level must be one below its parent.
	apfs_oid_t oid = child;
	for (; depth < (int32)kMaxDepth; depth++, expectedLevel--) {
		status_t status = _ReadLevel(depth, oid, fInner);
		if (status != B_OK)
			return status;
		if (fInner.Level() != expectedLevel) {
			ERROR("B-tree node %" B_PRIu64 " has level %u, expected %"
				B_PRId32 "\n", oid, fInner.Level(), expectedLevel);
			return B_BAD_DATA;
		}

		fPath[depth].oid = oid;
		fPath[depth].index = 0;
		fPath[depth].count = fInner.CountKeys();

		if (fInner.IsLeaf()) {
			memcpy(fLeaf.Buffer(), fInner.Buffer(),
				fTree->fContainer->BlockSize());
			status = fLeaf.Validate(&fTree->fInfo.fixed);
			if (status != B_OK)
				return status;
			fLeafDepth = depth;
			return B_OK;
		}

		if (fInner.CountKeys() == 0)
			return B_BAD_DATA;
		status = fInner.ChildAt(0, &oid);
		if (status != B_OK)
			return status;
	}
	return B_BAD_DATA;
}


status_t
BTreeIterator::_NextLeaf()
{
	// Climb until a level has an entry to the right, then go down its
	// leftmost path. Empty leaves are skipped.
	for (uint32 guard = 0; guard < 1000000; guard++) {
		int32 depth = fLeafDepth - 1;
		while (depth >= 0 && fPath[depth].index + 1 >= fPath[depth].count)
			depth--;

		if (depth < 0) {
			fAtEnd = true;
			return B_ENTRY_NOT_FOUND;
		}

		status_t status = _ReadLevel(depth, fPath[depth].oid, fInner);
		if (status != B_OK)
			return status;
		fPath[depth].index++;
		apfs_oid_t child;
		status = fInner.ChildAt(fPath[depth].index, &child);
		if (status != B_OK)
			return status;

		status = _DescendLeftmost(depth + 1, child, fInner.Level() - 1);
		if (status != B_OK)
			return status;
		if (fPath[fLeafDepth].count > 0)
			return B_OK;
	}
	return B_BAD_DATA;
}


status_t
BTreeIterator::GetNext(const uint8** _key, uint16* _keyLength,
	const uint8** _value, uint16* _valueLength)
{
	if (fInitStatus != B_OK)
		return fInitStatus;
	if (fAtEnd)
		return B_ENTRY_NOT_FOUND;

	Level& leaf = fPath[fLeafDepth];
	if (leaf.index >= leaf.count) {
		status_t status = _NextLeaf();
		if (status != B_OK)
			return status;
	}

	Level& current = fPath[fLeafDepth];
	status_t status = fLeaf.KeyAt(current.index, _key, _keyLength);
	if (status != B_OK)
		return status;

	status = fLeaf.ValueAt(current.index, _value, _valueLength);
	if (status == B_ENTRY_NOT_FOUND) {
		// A ghost: key without value.
		*_value = NULL;
		*_valueLength = 0;
	} else if (status != B_OK)
		return status;

	current.index++;
	return B_OK;
}
