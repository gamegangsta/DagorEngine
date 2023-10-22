#include <de3_interface.h>
#include <de3_entityFilter.h>
#include <de3_objEntity.h>
#include <oldEditor/de_interface.h>
#include <oldEditor/de_workspace.h>

class EntityFilterService : public IEditorService, public IObjEntityFilter
{
public:
  EntityFilterService(const char *subtype_str, const char *_name, const char *user_name, bool f_coll = true, bool f_export = true,
    bool f_render = true)
  {
    visible = true;
    fRender = f_render;
    fExport = f_export;
    fCollision = f_coll;
    name = _name;
    userName = user_name;
    bit = IObjEntity::registerSubTypeId(subtype_str);
    bit = (bit > 31) ? 0 : (1 << bit);
  }

  // IEditorService interface
  virtual const char *getServiceName() const { return name; }
  virtual const char *getServiceFriendlyName() const { return userName; }

  virtual void setServiceVisible(bool vis)
  {
    if (fRender)
    {
      visible = vis;
      applyFiltering(STMASK_TYPE_RENDER, visible);
    }
  }
  virtual bool getServiceVisible() const { return visible; }

  virtual void actService(float dt) {}
  virtual void beforeRenderService() {}
  virtual void renderService() {}
  virtual void renderTransService() {}
  virtual void onBeforeReset3dDevice() {}

  virtual void *queryInterfacePtr(unsigned huid)
  {
    RETURN_INTERFACE(huid, IObjEntityFilter);
    return NULL;
  }

  virtual bool allowFiltering(int mask_type)
  {
    switch (mask_type)
    {
      case STMASK_TYPE_RENDER: return fRender;
      case STMASK_TYPE_COLLISION: return fCollision;
      case STMASK_TYPE_EXPORT: return fExport;
    }
    return false;
  }
  virtual void applyFiltering(int mask_type, bool use)
  {
    int mask = IObjEntityFilter::getSubTypeMask(mask_type);
    if (use)
      mask |= bit;
    else
      mask &= ~bit;

    IObjEntityFilter::setSubTypeMask(mask_type, mask);
  }
  virtual bool isFilteringActive(int mask_type) { return (IObjEntityFilter::getSubTypeMask(mask_type) & bit) ? true : false; }

protected:
  bool visible;
  bool fRender, fExport, fCollision;
  unsigned bit;
  const char *name;
  const char *userName;
};


void init_entity_filter_service()
{
  if (!IDaEditor3Engine::get().checkVersion())
    return;

  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("single_ent", "_singleEnt", "(filter) Distinct entities"));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("poly_tile", "_tileLcPoly", "(filter) Generated by polygon"));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("mask_tile", "_tileMask", "(filter) Generated by landclass map"));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("spline_cls", "_genSpline", "(filter) Generated by spline"));
  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("road_obj", "_roadObj", "(filter) Road geom and objects"));
  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("hmap_obj", "_hmapObj", "(filter) HeightMap"));
  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("grass_obj", "_grassObj", "(filter) Grass"));
  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("csg_obj", "_cgsObj", "(filter) CSG"));
  IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("water_surf", "_waterSurf", "(filter) Water surface geom"));

  bool requireTileTex, hasColorTex, hasLightmapTex, useMeshSurface, useNormalMap;
  DAGORED2->getWorkspace().getHmapSettings(requireTileTex, hasColorTex, hasLightmapTex, useMeshSurface, useNormalMap);

  if (useMeshSurface)
  {
    IDaEditor3Engine::get().registerService(new (inimem) EntityFilterService("lmesh_obj", "_lmeshObj", "(filter) LandMesh"));
    IDaEditor3Engine::get().registerService(
      new (inimem) EntityFilterService("lmesh_obj_det", "_lmeshObjDet", "(filter) LandMesh for detailed area"));
  }

  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("collision", "_collision", "(filter) Show entity collision geom", false, false, true));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("rend_ent_geom", "_rend_ent_geom", "(filter) Show entity visual geom", false, false, true));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("lt_geom", "_rend_lt_geom", "(filter) Light objects geom", false, true, true));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("snow_obj", "_rend_snow_obj", "(filter) Snow sources", false, true, true));
  IDaEditor3Engine::get().registerService(
    new (inimem) EntityFilterService("navmesh", "_navmesh", "(filter) Show navigation mesh", false, false, true));

  EntityFilterService *groundObjFilterSrv =
    new (inimem) EntityFilterService("ground_obj", "_rend_ground_obj", "(filter) Show ground objects", false, false, true);
  groundObjFilterSrv->setServiceVisible(false); // not visible by default
  IDaEditor3Engine::get().registerService(groundObjFilterSrv);
}