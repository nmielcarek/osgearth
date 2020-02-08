/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2018 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include <osgEarth/Notify>
#include <osgEarth/MapNode>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/Feature>
#include <osgEarth/TerrainTileModelFactory>
#include <osgEarth/LandCover>
#include <osgEarthSplat/GroundCoverLayer>
#include <osgEarthSplat/NoiseTextureFactory>


#define LC "[exportgroundcover] "

using namespace osgEarth;
using namespace osgEarth::Splat;
using namespace osgEarth::Util;

int
usage(const char* name, const std::string& error)
{
    OE_NOTICE 
        << "Error: " << error
        << "\nUsage:"
        << "\n" << name << " file.earth"
        << "\n  --layer layername                     ; name of GroundCover layer"
        << "\n  --extents swlong swlat nelong nelat   ; extents in degrees"
        << "\n  --out out.shp                         ; output features"
        << std::endl;

    return 0;
}

// GLSL functions :)
double fract(double x)
{
    return fmod(x, 1.0);
}


int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc,argv);

    std::string layername;
    if (!arguments.read("--layer", layername))
        return usage(argv[0], "Missing --layer");

    double xmin, ymin, xmax, ymax;
    if (!arguments.read("--extents", xmin, ymin, xmax, ymax))
        return usage(argv[0], "Missing --extents");
    GeoExtent extent(SpatialReference::get("wgs84"), xmin, ymin, xmax, ymax);

    std::string outfile;
    if (!arguments.read("--out", outfile))
        return usage(argv[0], "Missing --out");

    osg::ref_ptr<MapNode> mapNode = MapNode::load(arguments);
    if (!mapNode.valid())
        return usage(argv[0], "No earth file");

    // find layers
    const Map* map = mapNode->getMap();

    GroundCoverLayer* gclayer = map->getLayerByName<GroundCoverLayer>(layername);
    if (!gclayer)
        return usage(argv[0], "GroundCover layer not found in map");

    LandCoverLayer* lclayer = map->getLayer<LandCoverLayer>();
    //if (!lclayer)
    //    return usage(argv[0], "No LandCoverLayer found in the map");

    LandCoverDictionary* lcdict = map->getLayer<LandCoverDictionary>();
    if (lclayer && !lcdict)
        return usage(argv[0], "No LandCoverDictionary found in the map");

    ImageLayer* masklayer = gclayer->getMaskLayer(); // could be null...?

    // open layers
    if (lclayer && lclayer->open().isError())
        return usage(argv[0], lclayer->getStatus().toString());

    if (masklayer && masklayer->open().isError())
        return usage(argv[0], masklayer->getStatus().toString());

    if (gclayer->open().isError())
        return usage(argv[0], gclayer->getStatus().toString());

    // create output shapefile
    osg::ref_ptr<FeatureProfile> outProfile = new FeatureProfile(extent);
    FeatureSchema outSchema;
    outSchema["tilekey"] = ATTRTYPE_STRING;
    osg::ref_ptr<OGRFeatureSource> outfs = new OGRFeatureSource();
    outfs->setOGRDriver("ESRI Shapefile");
    outfs->setURL(outfile);
    if (outfs->create(outProfile.get(), outSchema, Geometry::TYPE_POINTSET, NULL).isError())
        return usage(argv[0], outfs->getStatus().toString());

    // create noise texture
    NoiseTextureFactory noise;
    osg::ref_ptr<osg::Texture> noiseTexture = noise.create(256u, 4u);
    const int NOISE_SMOOTH = 0;
    const int NOISE_RANDOM = 1;
    const int NOISE_RANDOM_2 = 2;
    const int NOISE_CLUMPY = 3;
    ImageUtils::PixelReader readNoise(noiseTexture->getImage(0));

    // find all intersecting tile keys
    std::vector<TileKey> keys;
    map->getProfile()->getIntersectingTiles(extent, gclayer->getLOD(), keys);
    if (keys.empty())
        return usage(argv[0], "Invalid extent");

    // set up the factory
    CreateTileModelFilter layerFilter;
    if (lclayer)
        layerFilter.layers().insert(lclayer->getUID());
    if (masklayer)
        layerFilter.layers().insert(masklayer->getUID());
    layerFilter.layers().insert(gclayer->getUID());
    TerrainTileModelFactory factory(const_cast<const MapNode*>(mapNode.get())->options().terrain().get());

    int count = 0;
    osg::Vec4f landCover, mask;

    for(std::vector<TileKey>::const_iterator i = keys.begin();
        i != keys.end();
        ++i)
    {
        ++count;
        std::cout << "\r" << count << "/" << keys.size() << std::flush;

        const TileKey& key = *i;
        osg::ref_ptr<TerrainTileModel> model = factory.createTileModel(map, key, layerFilter, NULL, NULL);
        if (model.valid())
        {
            // for now, default to zone 0
            Zone* zone = gclayer->getZones()[0].get();
            if (zone)
            {
                GroundCover* groundcover = zone->getGroundCover();
                if (groundcover)
                {
                    // mask texture/matrix:
                    osg::Texture* maskTex = NULL;
                    osg::Matrix maskMat;
                    if (masklayer)
                    {
                        maskTex = model->getTexture(masklayer->getUID());
                        osg::RefMatrixf* r = model->getMatrix(masklayer->getUID());
                        if (r) maskMat = *r;
                    }
                    ImageUtils::PixelReader readMask(maskTex? maskTex->getImage(0) : NULL);

                    // landcover texture/matrix:
                    osg::Texture* lcTex = NULL;
                    osg::Matrixf lcMat;
                    if (lclayer)
                    {
                        lcTex = model->getLandCoverTexture();
                        if (!lcTex)
                        {
                            OE_WARN << "No land cover texture for this key..." << std::endl;
                            continue;
                        }
                        osg::RefMatrixf* r = model->getLandCoverTextureMatrix();
                        if (r) lcMat = *r;
                    }
                    ImageUtils::PixelReader readLandCover(lcTex ? lcTex->getImage(0) : NULL);


                    osg::Vec2f numInstances(128, 128); // TODO: fetch this.
                    unsigned numS = (unsigned)numInstances.x();
                    unsigned numT = (unsigned)numInstances.y();

                    osg::Vec2f offset, halfSpacing, tilec, shift;
                    osg::Vec4f noise(1,1,1,1);

                    PointSet* points = new PointSet();

                    for(unsigned t=0; t< numT; ++t)
                    {
                        for(unsigned s=0; s< numS; ++s)
                        {
                            float instanceID = (float)(t*numT + s);
                            
                            offset.set(
                                fmod(instanceID, (float)numS),
                                instanceID / (float)numT);

                            halfSpacing.set(
                                0.5/(float)numS,
                                0.5/(float)numT);

                            tilec.set(
                                halfSpacing.x() + offset.x()/numInstances.x(),
                                halfSpacing.y() + offset.y()/numInstances.y());

                            double u = (double)s / (double)(numS-1);
                            double v = (double)t / (double)(numT-1);
                            readNoise(noise, u, v);

                            shift.set(
                                fract(noise[NOISE_RANDOM]*5.5)*2.0-1.0,
                                fract(noise[NOISE_RANDOM_2]*5.5)*2.0-1.0);

                            tilec.x() += shift.x()*halfSpacing.x();
                            tilec.y() += shift.y()*halfSpacing.y();

                            // check the fill
                            if (noise[NOISE_SMOOTH] > groundcover->getFill())
                                continue;                         
                            else if (noise[NOISE_SMOOTH] > 0.0)
                                noise[NOISE_SMOOTH] /= groundcover->getFill();

                            // check the land cover
                            if (lcTex)
                            {
                                double u_lc = lcMat(0, 0)*u + lcMat(3, 0);
                                double v_lc = lcMat(1, 1)*v + lcMat(3, 1);
                                readLandCover(landCover, u_lc, v_lc);
                                const LandCoverClass* lcclass = lcdict->getClassByValue((int)landCover.r());
                                if (lcclass == NULL)
                                    continue;
                                const GroundCoverBiome* biome = groundcover->getBiome(lcclass);
                                if (!biome)
                                    continue;
                            }

                            // check the mask
                            if (maskTex)
                            {
                                double u_mask = maskMat(0,0)*u + maskMat(3,0);
                                double v_mask = maskMat(1,1)*v + maskMat(3,1);
                                readMask(mask, u_mask, v_mask);
                                if (mask.r() > 0.0)
                                    continue;
                            }

                            // keeper
                            points->push_back(osg::Vec3d(
                                key.getExtent().xMin() + tilec.x()*key.getExtent().width(),
                                key.getExtent().yMin() + tilec.y()*key.getExtent().height(),
                                0.0));                     
                        }
                    }

                    // write a multipoint feature for the tile
                    if (points->size() > 0)
                    {
                        osg::ref_ptr<Feature> feature = new Feature(points, key.getExtent().getSRS());
                        feature->set("tilekey", key.str());
                        outfs->insertFeature(feature.get());
                    }
                }
            }
        }
    }
    std::cout << std::endl;

    outfs->close();
}