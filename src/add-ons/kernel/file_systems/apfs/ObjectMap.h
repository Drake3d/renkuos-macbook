/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_OBJECT_MAP_H
#define APFS_OBJECT_MAP_H


#include "apfs.h"


class BTree;
class Container;


// An object map: finds where a virtual object is stored at a given
// transaction.
class ObjectMap {
public:
								ObjectMap(Container* container);
								~ObjectMap();

			// omap is the physical address of the omap_phys_t object.
			status_t			SetTo(apfs_paddr_t omap);

			// Finds the newest mapping of oid whose transaction id is
			// <= xid. Deleted mappings count as not found.
			status_t			Lookup(apfs_oid_t oid, apfs_xid_t xid,
									apfs_omap_val* value);

private:
			Container*			fContainer;
			BTree*				fTree;
};


#endif	// APFS_OBJECT_MAP_H
