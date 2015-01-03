/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "MPGeometry"

#include <osg/Version>
#include <osgUtil/MeshOptimizers>
#include <iterator>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>

#include <osgUtil/IncrementalCompileOperation>

using namespace osg;
using namespace osgEarth::Drivers::MPTerrainEngine;
using namespace osgEarth;

#define LC "[MPGeometry] "


MPGeometry::MPGeometry(const TileKey& key, const MapFrame& frame, int imageUnit) : 
osg::Geometry    ( ),
_frame           ( frame ),
_imageUnit       ( imageUnit )
{
    _supportsGLSL = Registry::capabilities().supportsGLSL();

    unsigned tw, th;
    key.getProfile()->getNumTiles(key.getLOD(), tw, th);
    _tileKeyValue.set( key.getTileX(), th-key.getTileY()-1.0f, key.getLOD(), -1.0f );

    _imageUnitParent = _imageUnit + 1; // temp

    _elevUnit = _imageUnit + 2; // temp

    // establish uniform name IDs.
    _tileKeyUniformNameID      = osg::Uniform::getNameID( "oe_tile_key" );
    _birthTimeUniformNameID    = osg::Uniform::getNameID( "oe_tile_birthtime" );
    _uidUniformNameID          = osg::Uniform::getNameID( "oe_layer_uid" );
    _orderUniformNameID        = osg::Uniform::getNameID( "oe_layer_order" );
    _opacityUniformNameID      = osg::Uniform::getNameID( "oe_layer_opacity" );

    _texMatrixUniformNameID    = osg::Uniform::getNameID( "oe_layer_texMatrix" );

    // we will set these later (in TileModelCompiler)
    this->setUseVertexBufferObjects(true);
    this->setUseDisplayList(false);
}


void
MPGeometry::drawPrimitivesImplementation(osg::RenderInfo& renderInfo) const
{
    // check the map frame to see if it's up to date
    if ( _frame.needsSync() )
    {
        // this lock protects a MapFrame sync when we have multiple DRAW threads.
        Threading::ScopedMutexLock exclusive( _frameSyncMutex );

        if ( _frame.needsSync() && _frame.sync() ) // always double check
        {
            // This should only happen is the layer ordering changes;
            // If layers are added or removed, the Tile gets rebuilt and
            // the point is moot.
            std::vector<Layer> reordered;
            const ImageLayerVector& layers = _frame.imageLayers();
            reordered.reserve( layers.size() );
            for( ImageLayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i )
            {
                std::vector<Layer>::iterator j = std::find( _layers.begin(), _layers.end(), i->get()->getUID() );
                if ( j != _layers.end() )
                    reordered.push_back( *j );
            }
            _layers.swap( reordered );
        }
    }

    unsigned layersDrawn = 0;

    // access the GL extensions interface for the current GC:
    const osg::Program::PerContextProgram* pcp = 0L;
    osg::ref_ptr<osg::GL2Extensions> ext;
    unsigned contextID;
    osg::State& state = *renderInfo.getState();

    if (_supportsGLSL)
    {
        contextID = state.getContextID();
        ext = osg::GL2Extensions::Get( contextID, true );
        pcp = state.getLastAppliedProgramObject();
    }

    // cannot store these in the object since there could be multiple GCs (and multiple
    // PerContextPrograms) at large
    GLint tileKeyLocation       = -1;
    GLint birthTimeLocation     = -1;
    GLint opacityLocation       = -1;
    GLint uidLocation           = -1;
    GLint orderLocation         = -1;
    GLint texMatrixLocation     = -1;

    // The PCP can change (especially in a VirtualProgram environment). So we do need to
    // requery the uni locations each time unfortunately. TODO: explore optimizations.
    if ( pcp )
    {
        tileKeyLocation      = pcp->getUniformLocation( _tileKeyUniformNameID );
        birthTimeLocation    = pcp->getUniformLocation( _birthTimeUniformNameID );
        opacityLocation      = pcp->getUniformLocation( _opacityUniformNameID );
        uidLocation          = pcp->getUniformLocation( _uidUniformNameID );
        orderLocation        = pcp->getUniformLocation( _orderUniformNameID );
        texMatrixLocation    = pcp->getUniformLocation( _texMatrixUniformNameID );
    }
    
    // apply the tilekey uniform once.
    if ( tileKeyLocation >= 0 )
    {
        ext->glUniform4fv( tileKeyLocation, 1, _tileKeyValue.ptr() );
    }

    // set the "birth time" - i.e. the time this tile last entered the scene in the current GC.
    if ( birthTimeLocation >= 0 )
    {
        PerContextData& pcd = _pcd[contextID];
        if ( pcd.birthTime < 0.0f )
        {
            const osg::FrameStamp* stamp = state.getFrameStamp();
            if ( stamp )
            {
                pcd.birthTime = stamp->getReferenceTime();
            }
        }
        ext->glUniform1f( birthTimeLocation, pcd.birthTime );
    }

//#ifndef OSG_GLES2_AVAILABLE
//    if ( renderColor )
//    {
//        // emit a default terrain color since we're not binding a color array:
//        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
//    }
//#endif

    float prevOpacity = -1.0f;

    for(std::vector<Layer>::const_iterator i = _layers.begin(); i != _layers.end(); ++i)
    {
        const ImageLayer* imageLayer = i->_imageLayer.get();
        if ( imageLayer->getVisible() && imageLayer->getOpacity() > 0.0f )
        {
            // bind the proper unit:
            state.setActiveTextureUnit( 0 ); // todo: color binding unit

            // bind the color texture:
            if ( i->_tex.valid() )
                i->_tex->apply( state );  
            else
                OE_WARN << LC << "NO texture!\n";

            // apply the color texture matrix uniform:
            if ( i->_texMatrix.valid() )
                ext->glUniformMatrix4fv( texMatrixLocation, 1, GL_FALSE, i->_texMatrix->ptr() );
            else
                OE_WARN << LC << "NO tex matrix!\n";
            
            // apply uniform values:
            if ( pcp )
            {
                // apply opacity:
                if ( opacityLocation >= 0 )
                {
                    float opacity = imageLayer->getOpacity();
                    if ( opacity != prevOpacity )
                    {
                        ext->glUniform1f( opacityLocation, (GLfloat)opacity );
                        prevOpacity = opacity;
                    }
                }

                // assign the layer UID:
                if ( uidLocation >= 0 )
                {
                    ext->glUniform1i( uidLocation, (GLint)imageLayer->getUID() );
                }

                // assign the layer order:
                if ( orderLocation >= 0 )
                {
                    ext->glUniform1i( orderLocation, (GLint)layersDrawn );
                }

                // draw the primitive sets.
                for(unsigned int primitiveSetNum=0; primitiveSetNum!=_primitives.size(); ++primitiveSetNum)
                {
                    const osg::PrimitiveSet* primitiveset = _primitives[primitiveSetNum].get();
                    if ( primitiveset )
                    {
                        primitiveset->draw(state, true);
                    }
                    else
                    {
                        OE_WARN << LC << "Strange, MPGeometry had a 0L primset" << std::endl;
                    }
                }

                ++layersDrawn;
            }
        }
    }

    // Draw when there are no textures:
    if ( layersDrawn == 0 )
    {
        if ( opacityLocation >= 0 )
            ext->glUniform1f( opacityLocation, (GLfloat)1.0f );
        if ( uidLocation >= 0 )
            ext->glUniform1i( uidLocation, (GLint)-1 );
        if ( orderLocation >= 0 )
            ext->glUniform1i( orderLocation, (GLint)0 );

        for(unsigned int primitiveSetNum=0; primitiveSetNum!=_primitives.size(); ++primitiveSetNum)
        {
            const osg::PrimitiveSet* primitiveset = _primitives[primitiveSetNum].get();
            if ( primitiveset )
            {
                primitiveset->draw(state, true);
            }
            else
            {
                OE_WARN << LC << "Strange, MPGeometry had a 0L primset" << std::endl;
            }
        }
    }
}


#if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
#    define COMPUTE_BOUND computeBoundingBox
#else
#    define COMPUTE_BOUND computeBound
#endif

#if OSG_VERSION_GREATER_OR_EQUAL(3,1,8)
#   define GET_ARRAY(a) (a)
#else
#   define GET_ARRAY(a) (a).array
#endif

osg::BoundingBox
MPGeometry:: COMPUTE_BOUND() const
{
    osg::BoundingBox bbox = osg::Geometry:: COMPUTE_BOUND ();
    {
        // update the uniform.
        Threading::ScopedMutexLock exclusive(_frameSyncMutex);
        _tileKeyValue.w() = bbox.radius();
    }
    return bbox;
}

void
MPGeometry::validate()
{
    unsigned numVerts = getVertexArray()->getNumElements();

    for(unsigned i=0; i < _primitives.size(); ++i)
    {
        osg::DrawElements* de = static_cast<osg::DrawElements*>(_primitives[i].get());
        if ( de->getMode() != GL_TRIANGLES )
        {
            OE_WARN << LC << "Invalid primitive set - not GL_TRIANGLES" << std::endl;
            _primitives.clear();
        }

        else if ( de->getNumIndices() % 3 != 0 )
        {
            OE_WARN << LC << "Invalid primitive set - wrong number of indicies" << std::endl;
            //_primitives.clear();
            osg::DrawElementsUShort* deus = static_cast<osg::DrawElementsUShort*>(de);
            int extra = de->getNumIndices() % 3;
            deus->resize(de->getNumIndices() - extra);
            OE_WARN << LC << "   ..removed " << extra << " indices" << std::endl;
            //return;
        }
        else
        {
            for( unsigned j=0; j<de->getNumIndices(); ++j ) 
            {
                unsigned index = de->index(j);
                if ( index >= numVerts )
                {
                    OE_WARN << LC << "Invalid primitive set - index out of bounds" << std::endl;
                    _primitives.clear();
                    return;
                }
            }
        }
    }
}


void 
MPGeometry::releaseGLObjects(osg::State* state) const
{
    osg::Geometry::releaseGLObjects( state );

    for(std::vector<Layer>::const_iterator i = _layers.begin(); i != _layers.end(); ++i)
    {
        if ( i->_tex.valid() )
        {
            i->_tex->releaseGLObjects( state );
        }
    }
}


void
MPGeometry::resizeGLObjectBuffers(unsigned maxSize)
{
    osg::Geometry::resizeGLObjectBuffers( maxSize );

    if ( _pcd.size() < maxSize )
    {
        _pcd.resize(maxSize);
    }
}


void 
MPGeometry::compileGLObjects(osg::RenderInfo& renderInfo) const
{
    osg::Geometry::compileGLObjects( renderInfo );
    
    osg::State& state = *renderInfo.getState();
    unsigned contextID = state.getContextID();
    GLBufferObject::Extensions* extensions = GLBufferObject::getExtensions(contextID, true);
    if (!extensions)
        return;

    MPGeometry* ncthis = const_cast<MPGeometry*>(this);

    for(std::vector<Layer>::iterator i = _layers.begin(); i != _layers.end(); ++i)
    {
        if ( i->_tex.valid() )
            i->_tex->apply( state );
    }

    // unbind the BufferObjects
    extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
    extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
}


#if OSG_MIN_VERSION_REQUIRED(3,1,8)
void
MPGeometry::drawVertexArraysImplementation(osg::RenderInfo& renderInfo) const
{
    State& state = *renderInfo.getState();

    bool handleVertexAttributes = !_vertexAttribList.empty();

    ArrayDispatchers& arrayDispatchers = state.getArrayDispatchers();

    arrayDispatchers.reset();
    arrayDispatchers.setUseVertexAttribAlias(state.getUseVertexAttributeAliasing());

    arrayDispatchers.activateNormalArray(_normalArray.get());
    arrayDispatchers.activateColorArray(_colorArray.get());
    arrayDispatchers.activateSecondaryColorArray(_secondaryColorArray.get());
    arrayDispatchers.activateFogCoordArray(_fogCoordArray.get());

    if (handleVertexAttributes)
    {
        for(unsigned int unit=0;unit<_vertexAttribList.size();++unit)
        {
            arrayDispatchers.activateVertexAttribArray(unit, _vertexAttribList[unit].get());
        }
    }

    // dispatch any attributes that are bound overall
    arrayDispatchers.dispatch(osg::Array::BIND_OVERALL,0);

    state.lazyDisablingOfVertexAttributes();

    // set up arrays
    if( _vertexArray.valid() )
        state.setVertexPointer(_vertexArray.get());

    if (_normalArray.valid() && _normalArray->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setNormalPointer(_normalArray.get());

    if (_colorArray.valid() && _colorArray->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setColorPointer(_colorArray.get());

    if (_secondaryColorArray.valid() && _secondaryColorArray->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setSecondaryColorPointer(_secondaryColorArray.get());

    if (_fogCoordArray.valid() && _fogCoordArray->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setFogCoordPointer(_fogCoordArray.get());

    for(unsigned int unit=0;unit<_texCoordList.size();++unit)
    {
        const Array* array = _texCoordList[unit].get();
        if (array)
        {
            state.setTexCoordPointer(unit,array);
        }
    }

    if ( handleVertexAttributes )
    {
        for(unsigned int index = 0; index < _vertexAttribList.size(); ++index)
        {
            const Array* array = _vertexAttribList[index].get();
            if (array && array->getBinding()==osg::Array::BIND_PER_VERTEX)
            {
                if (array->getPreserveDataType())
                {
                    GLenum dataType = array->getDataType();
                    if (dataType==GL_FLOAT) state.setVertexAttribPointer( index, array );
                    else if (dataType==GL_DOUBLE) state.setVertexAttribLPointer( index, array );
                    else state.setVertexAttribIPointer( index, array );
                }
                else
                {
                    state.setVertexAttribPointer( index, array );
                }
            }
        }
    }

    state.applyDisablingOfVertexAttributes();
}

#else

void
MPGeometry::drawVertexArraysImplementation(osg::RenderInfo& renderInfo) const
{
    State& state = *renderInfo.getState();
    bool handleVertexAttributes = !_vertexAttribList.empty();

    ArrayDispatchers& arrayDispatchers = state.getArrayDispatchers();

    arrayDispatchers.reset();
    arrayDispatchers.setUseVertexAttribAlias(useFastPath && state.getUseVertexAttributeAliasing());
    arrayDispatchers.setUseGLBeginEndAdapter(!useFastPath);

    arrayDispatchers.activateNormalArray(_normalData.binding, _normalData.array.get(), _normalData.indices.get());
    arrayDispatchers.activateColorArray(_colorData.binding, _colorData.array.get(), _colorData.indices.get());
    arrayDispatchers.activateSecondaryColorArray(_secondaryColorData.binding, _secondaryColorData.array.get(), _secondaryColorData.indices.get());
    arrayDispatchers.activateFogCoordArray(_fogCoordData.binding, _fogCoordData.array.get(), _fogCoordData.indices.get());

    if (handleVertexAttributes)
    {
        for(unsigned int unit=0;unit<_vertexAttribList.size();++unit)
        {
            arrayDispatchers.activateVertexAttribArray(_vertexAttribList[unit].binding, unit, _vertexAttribList[unit].array.get(), _vertexAttribList[unit].indices.get());
        }
    }

    // dispatch any attributes that are bound overall
    arrayDispatchers.dispatch(BIND_OVERALL,0);

    state.lazyDisablingOfVertexAttributes();

    if (useFastPath)
    {
        // set up arrays
        if( _vertexData.array.valid() )
            state.setVertexPointer(_vertexData.array.get());

        if (_normalData.binding==BIND_PER_VERTEX && _normalData.array.valid())
            state.setNormalPointer(_normalData.array.get());

        if (_colorData.binding==BIND_PER_VERTEX && _colorData.array.valid())
            state.setColorPointer(_colorData.array.get());

        if (_secondaryColorData.binding==BIND_PER_VERTEX && _secondaryColorData.array.valid())
            state.setSecondaryColorPointer(_secondaryColorData.array.get());

        if (_fogCoordData.binding==BIND_PER_VERTEX && _fogCoordData.array.valid())
            state.setFogCoordPointer(_fogCoordData.array.get());

        for(unsigned int unit=0;unit<_texCoordList.size();++unit)
        {
            const Array* array = _texCoordList[unit].array.get();
            if (array) state.setTexCoordPointer(unit,array);
        }

        if( handleVertexAttributes )
        {
            for(unsigned int index = 0; index < _vertexAttribList.size(); ++index )
            {
                const Array* array = _vertexAttribList[index].array.get();
                const AttributeBinding ab = _vertexAttribList[index].binding;
                if( ab == BIND_PER_VERTEX && array )
                {
                    state.setVertexAttribPointer( index, array, _vertexAttribList[index].normalize );
                }
            }
        }
    }
    else
    {
        for(unsigned int unit=0;unit<_texCoordList.size();++unit)
        {
            arrayDispatchers.activateTexCoordArray(BIND_PER_VERTEX, unit, _texCoordList[unit].array.get(), _texCoordList[unit].indices.get());
        }

        arrayDispatchers.activateVertexArray(BIND_PER_VERTEX, _vertexData.array.get(), _vertexData.indices.get());
    }

    state.applyDisablingOfVertexAttributes();
}

#endif

void
MPGeometry::drawImplementation(osg::RenderInfo& renderInfo) const
{
    State& state = *renderInfo.getState();
    bool checkForGLErrors = state.getCheckForGLErrors() == osg::State::ONCE_PER_ATTRIBUTE;
    if ( checkForGLErrors ) state.checkGLErrors("start of MPGeometry::drawImplementation()");

    drawVertexArraysImplementation( renderInfo );
    drawPrimitivesImplementation( renderInfo );

    if ( checkForGLErrors ) state.checkGLErrors("end of MPGeometry::drawImplementation()");
    
    // unbind the VBO's if any are used.
    state.unbindVertexBufferObject();
    state.unbindElementBufferObject();
}
