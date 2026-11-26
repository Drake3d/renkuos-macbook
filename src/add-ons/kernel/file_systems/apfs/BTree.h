/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_BTREE_H
#define APFS_BTREE_H


#include "apfs.h"


class Container;
class ObjectMap;


// Compares an on-disk key with a search target: < 0 if the key sorts
// before the target, 0 if equal, > 0 if after.
typedef int (*apfs_key_compare)(const void* key, uint16 keyLength,
	const void* target, void* context);


// One B-tree node in memory, with bounds-checked access to its table of
// contents, keys and values.
class BTreeNode {
public:
								BTreeNode();
								~BTreeNode();

			status_t			Init(uint32 blockSize);
			// Checks the node read into Buffer(); info is the tree's fixed
			// information (from the root, or NULL when this is the root).
			status_t			Validate(const apfs_btree_info_fixed* info);

			uint8*				Buffer() const { return fBlock; }
			const apfs_btree_node_phys* Header() const
									{ return (const apfs_btree_node_phys*)
										fBlock; }
			const apfs_btree_info* Info() const;

			uint32				CountKeys() const
									{ return Header()->KeyCount(); }
			uint16				Level() const { return Header()->Level(); }
			bool				IsLeaf() const { return Header()->IsLeaf(); }

			status_t			KeyAt(uint32 index, const uint8** _key,
									uint16* _length) const;
			// Returns B_ENTRY_NOT_FOUND for a ghost (key without value).
			status_t			ValueAt(uint32 index, const uint8** _value,
									uint16* _length) const;
			status_t			ChildAt(uint32 index,
									apfs_oid_t* _child) const;

private:
			uint8*				fBlock;
			uint32				fBlockSize;
			apfs_btree_info_fixed fFixed;
			uint32				fTableStart;
			uint32				fKeyStart;
			uint32				fValueEnd;
};


// Read-only access to one B-tree. Child links are resolved according to
// the tree flags: physical, ephemeral (checkpoint), or virtual (object map).
class BTree {
public:
								BTree(Container* container,
									ObjectMap* objectMap, apfs_xid_t xid);
								~BTree();

			// rootType is the "*_tree_type" field that names the root's
			// storage (OBJ_PHYSICAL, OBJ_EPHEMERAL or OBJ_VIRTUAL).
			status_t			SetTo(apfs_oid_t root, uint32 rootType,
									uint32 expectedSubtype);

			const apfs_btree_info& Info() const { return fInfo; }
			uint32				Flags() const
									{ return fInfo.fixed.Flags(); }

			// Finds the entry with the largest key that is <= target.
			// Copies its key and value (if the buffers are given; lengths
			// in/out). Returns B_ENTRY_NOT_FOUND when every key is larger.
			status_t			FindLessOrEqual(const void* target,
									apfs_key_compare compare, void* context,
									void* key, uint16* _keyLength,
									void* value, uint16* _valueLength);

private:
			status_t			_ReadNode(apfs_oid_t oid, uint32 storage,
									BTreeNode& node);

			Container*			fContainer;
			ObjectMap*			fObjectMap;
			apfs_xid_t			fTransactionID;
			apfs_oid_t			fRoot;
			uint32				fRootStorage;
			uint32				fSubtype;
			apfs_btree_info		fInfo;
};


#endif	// APFS_BTREE_H
