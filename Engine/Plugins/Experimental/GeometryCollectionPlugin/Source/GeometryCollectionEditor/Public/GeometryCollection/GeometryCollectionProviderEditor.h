// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GeometryCollection/GeometryCollectionCache.h"

class FTargetCacheProviderEditor : public ITargetCacheProvider
{
public:

	virtual UGeometryCollectionCache* GetCacheForCollection(UGeometryCollection* InCollection) override;

};