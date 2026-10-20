/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX LIB

#include "ob_geo_func_symdifference_helper.ipp"

using namespace oceanbase::common;
namespace oceanbase
{
namespace common
{

int ob_geo_func_symdifference_eval_wkb_geog(const common::ObGeometry *g1, const common::ObGeometry *g2,
    const ObGeoEvalCtx &context, ObGeometry *&result);

// tree cartesian polygon
OB_GEO_CART_TREE_FUNC_BEGIN(ObGeoFuncSymDifferenceImpl, ObCartesianPolygon, ObCartesianPolygon, ObGeometry *)
{
  return apply_bg_symdifference<ObCartesianPolygon, ObCartesianPolygon, ObCartesianMultipolygon>(g1, g2, context, result);
} OB_GEO_FUNC_END;

OB_GEO_CART_TREE_FUNC_BEGIN(ObGeoFuncSymDifferenceImpl, ObCartesianPolygon, ObCartesianMultipolygon, ObGeometry *)
{
  return apply_bg_symdifference<ObCartesianPolygon, ObCartesianMultipolygon, ObCartesianMultipolygon>(g1, g2, context, result);
} OB_GEO_FUNC_END;

OB_GEO_CART_TREE_FUNC_BEGIN(ObGeoFuncSymDifferenceImpl, ObCartesianMultipolygon, ObCartesianPolygon, ObGeometry *)
{
  return apply_bg_symdifference<ObCartesianMultipolygon, ObCartesianPolygon, ObCartesianMultipolygon>(g1, g2, context, result);
} OB_GEO_FUNC_END;

static int ob_geo_func_symdifference_eval_tree(const common::ObGeometry *g1, const common::ObGeometry *g2,
    const ObGeoEvalCtx &context, ObGeometry *&result)
{
  int ret = OB_SUCCESS;
  if (g1->crs() != g2->crs()) {
    ret = OB_ERR_GIS_DIFFERENT_SRIDS;
  } else {
    switch (g1->crs()) {
    case common::ObGeoCRS::Cartesian:
      ret = ObGeoFuncSymDifferenceImpl::eval_tree_binary_cart(g1, g2, context, result);
      break;
    case common::ObGeoCRS::Geographic:
      ret = ObGeoFuncSymDifferenceImpl::eval_tree_binary_geog(g1, g2, context, result);
      break;
    default:
      ret = OB_ERR_GIS_INVALID_DATA;
      break;
    }
  }
  return ret;
}

extern int ob_geo_func_symdifference_eval_wkb_cart(const common::ObGeometry *g1, const common::ObGeometry *g2,
    const ObGeoEvalCtx &context, ObGeometry *&result);

// implement of outer class eval
// use an outer class to void implement templates in header files
int ObGeoFuncSymDifference::eval(const ObGeoEvalCtx &gis_context, ObGeometry *&result)
{
  return ObGeoFuncSymDifferenceImpl::eval_geo_func_split(gis_context, result,
      ob_geo_func_symdifference_eval_wkb_cart,
      ob_geo_func_symdifference_eval_wkb_geog,
      ob_geo_func_symdifference_eval_tree);
}

}  // namespace common
}  // namespace oceanbase
