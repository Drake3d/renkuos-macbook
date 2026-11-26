/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Object maps.
 * Source: Apple File System Reference, revision 2020-06-22, chapter
 * "Object Maps": "omap_phys_t", "omap_key_t", "omap_val_t", "Object Map
 * Value Flags", and "B-Trees" ("Key Comparison": object map B-trees are
 * sorted by object identifier and then by transaction identifier).
 */


#include "ObjectMap.h"

#include "BTree.h"
#include "Container.h"


static int
compare_omap_keys(const void* _key, uint16 keyLength, const void* _target,
	void* /*context*/)
{
	if (keyLength < sizeof(apfs_omap_key))
		return 1;

	const apfs_omap_key* key = (const apfs_omap_key*)_key;
	const apfs_omap_key* target = (const apfs_omap_key*)_target;

	if (key->ObjectID() != target->ObjectID())
		return key->ObjectID() < target->ObjectID() ? -1 : 1;
	if (key->TransactionID() != target->TransactionID())
		return key->TransactionID() < target->TransactionID() ? -1 : 1;
	return 0;
}


ObjectMap::ObjectMap(Container* container)
	:
	fContainer(container),
	fTree(NULL)
{
}


ObjectMap::~ObjectMap()
{
	delete fTree;
}


status_t
ObjectMap::SetTo(apfs_paddr_t omap)
{
	uint8* block = (uint8*)malloc(fContainer->BlockSize());
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter blockDeleter(block);

	status_t status = fContainer->ReadObject(omap, block,
		APFS_OBJECT_TYPE_OMAP);
	if (status != B_OK)
		return status;

	const apfs_omap_phys* map = (const apfs_omap_phys*)block;

	// The mapping tree's own links are never resolved through an object
	// map, so the tree needs none.
	delete fTree;
	fTree = new(std::nothrow) BTree(fContainer, NULL, 0);
	if (fTree == NULL)
		return B_NO_MEMORY;

	status = fTree->SetTo(map->Tree(), map->TreeType(), APFS_OBJECT_TYPE_OMAP);
	if (status != B_OK)
		return status;

	if (fTree->Info().fixed.KeySize() != sizeof(apfs_omap_key)
		|| fTree->Info().fixed.ValueSize() != sizeof(apfs_omap_val)) {
		ERROR("object map %" B_PRId64 " has unexpected key/value sizes\n",
			omap);
		return B_BAD_DATA;
	}
	return B_OK;
}


status_t
ObjectMap::Lookup(apfs_oid_t oid, apfs_xid_t xid, apfs_omap_val* value)
{
	if (fTree == NULL)
		return B_NO_INIT;

	apfs_omap_key target;
	target.oid = B_HOST_TO_LENDIAN_INT64(oid);
	target.xid = B_HOST_TO_LENDIAN_INT64(xid);

	apfs_omap_key key;
	uint16 keyLength = sizeof(key);
	uint16 valueLength = sizeof(*value);
	status_t status = fTree->FindLessOrEqual(&target, compare_omap_keys, NULL,
		&key, &keyLength, value, &valueLength);
	if (status != B_OK)
		return status;

	if (key.ObjectID() != oid
		|| (value->Flags() & APFS_OMAP_VAL_DELETED) != 0)
		return B_ENTRY_NOT_FOUND;
	return B_OK;
}
