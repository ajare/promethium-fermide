# Willpower AppLib

This is a helper library providing entity, map, state, and rendering systems built on Willpower and MPP.

## AnimationDatabase

This is a database which holds sprite-based animations.  Its purpose is to store AnimationSet resources, and provide
an easy way to retrieve and loop through frames.

## Entities

### Entity

Has a unique id, a name, and a list of properties (with lookup) that apply to it.

### EntityProperties

### EntityFacade

This class holds **Entity** instances of a given type, the idea being that it allows for optimised rendering, as each EntityFacade has
its own MPP renderer and data provider.  When *createEntity()* is called, it passes the new instance in an **EntityHandler** which sets
up the instance properties.  The class also handles updating entities, again by delegating to the **EntityHandler**.

### EntityManager

This class is the main management interface for **Entity** instances, and holds all the required **EntityFacade** instances as well.  The
idea is that the class lets the user create an **EntityFacade** given a given set of Entity types, and then when you call *EntityManager::createEntity()*,
you pass in the Entity type, and it delegates to the corresponding **EntityFacade**.  The class also handles update and rendering by iterating over its
EntityFacades.

### EntityHandler

This class manages individual **Entity** instances, setting them up, destroying them, and updating them.  **EntityFacade** delegates to it for this.  It
also stores a registry of entity properties.

### EntityRenderDataProvider

TODO

### ProtoEntityResourceDefinitionFactory

A subclass of **wp::application::resourcesystem::ResourceDefinitionFactory**, this is used to create **ProtoEntity** instance definitions.

### ProtoEntityDefaultDefinitionFactory

A subclass of **ProtoEntityResourceDefinitionFactory**, this is used to create **ProtoEntity** instance definitions.

### PhysicalStats

### VisualStats

## Map

### MapDefaultDefinitionFactory

### MapResourceDefinitionFactory

### MapTiledDefinitionFactory

### MapGeometryObjectAttributes

### MapTransitionData

## Model

### ModelInstance

## ProtoEntity

This is a Willpower **Resource**, intended to be subclassed by an application.  It has a pointer to an **EntityHandler**,
which is used to 

### ProtoEntityDefaultDefinitionFactory

### ProtoEntityResourceDefinitionFactory

## State

### StateTransitionData

### StateController

### StatePlay

### ThreadableLoadState

#### StateLoad

#### StateMapLoad

#### StateMapTransition

#### StateMapUnload

#### StateUnload




