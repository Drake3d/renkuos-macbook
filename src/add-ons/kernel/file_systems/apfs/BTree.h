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
	friend class BTreeIterator;
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
			// Finds the entry whose key compares equal to target.
			status_t			FindExact(const void* target,
									apfs_key_compare compare, void* context,
									void* value, uint16* _valueLength);

private:
			status_t			_ReadNode(apfs_oid_t oid, uint32 storage,
									BTreeNode& node);
			uint32				_ChildStorage() const;
			status_t			_Find(const void* target,
									apfs_key_compare compare, void* context,
									bool exact, void* key, uint16* _keyLength,
									void* value, uint16* _valueLength);

			Container*			fContainer;
			ObjectMap*			fObjectMap;
			apfs_xid_t			fTransactionID;
			apfs_oid_t			fRoot;
			uint32				fRootStorage;
			uint32				fSubtype;
			apfs_btree_info		fInfo;
};


// Walks the entries of a B-tree in key order, from a starting point. Nodes
// have no sibling links, so the iterator keeps the path from the root and
// climbs back up when a leaf is used up.
class BTreeIterator {
public:
								BTreeIterator(BTree* tree);
								~BTreeIterator();

			status_t			InitCheck() const { return fInitStatus; }

			// Positions before the first entry whose key is >= target.
			status_t			Seek(const void* target,
									apfs_key_compare compare, void* context);

			// Returns the next entry and moves past it. The pointers stay
			// valid until the next call. B_ENTRY_NOT_FOUND at the end.
			status_t			GetNext(const uint8** _key,
									uint16* _keyLength, const uint8** _value,
									uint16* _valueLength);

private:
	static	const uint32		kMaxDepth = 16;

			struct Level {
				apfs_oid_t		oid;
				uint32			index;
				uint32			count;
			};

			status_t			_ReadLevel(int32 depth, apfs_oid_t oid,
									BTreeNode& node);
			status_t			_DescendLeftmost(int32 depth,
									apfs_oid_t child, int32 expectedLevel);
			status_t			_NextLeaf();

			BTree*				fTree;
			BTreeNode			fLeaf;
			BTreeNode			fInner;
			Level				fPath[kMaxDepth];
			int32				fLeafDepth;
			bool				fAtEnd;
			status_t			fInitStatus;
};


#endif	// APFS_BTREE_H
