#include "Scene.h"

namespace ofx {

namespace blender {

Scene::Scene() {
	name = "unnamed scene";
	activeCamera = NULL;
	//setScale(10);
	doDebug = false;
	isFirstDebugEnable = true;
	bHasViewport = false;
	doLightning = false;
}

Scene::~Scene() {

}

void Scene::setDebug(bool state) {
	if(state && isFirstDebugEnable && activeCamera) {
		debugCam.setNearClip(.01);
		debugCam.setGlobalPosition(activeCamera->getGlobalPosition());
		debugCam.setGlobalOrientation(activeCamera->getGlobalOrientation());
		debugCam.setDistance(debugCam.getTarget().getGlobalPosition().distance(activeCamera->getGlobalPosition()));
		debugCam.setFov(activeCamera->camera.getFov());
	}
	doDebug = state;
	isFirstDebugEnable = false;
}

void Scene::toggleDebug() {
	setDebug(!doDebug);
}

bool Scene::isDebugEnabled() {
	return doDebug;
}

void Scene::update() {
	timeline.step();
	for(Object* obj: objects) {
		obj->scene = this;
		obj->update();
	}
}

struct sortByDistToCamera {

	sortByDistToCamera(ofVec3f cp) {
		camPos = cp;
	}

	ofVec3f camPos;
	inline bool operator() (const Mesh* m1, const Mesh* m2) {
		return (m1->getGlobalPosition().distanceSquared(camPos) > m2->getGlobalPosition().distanceSquared(camPos));
	}
};

void Scene::customDraw() {
	//camera
	ofCamera* camera = &debugCam;
	if(activeCamera) {

		activeCamera->updateCamPos();
		activeCamera->updateLens();

		if(!doDebug)
			camera = &activeCamera->camera;
		else
			debugCam.setFov(activeCamera->camera.getFov());
	}

	if(!bHasViewport)
		camera->begin();
	else
		camera->begin(viewport);

	//basics
	ofPushStyle();
	ofEnableNormalizedTexCoords();

	ofEnableDepthTest();

	//update the material properties
	//TODO: could be optimized

	for(Material* material: materials) {
		material->lights = lights;
		material->isLightningEnabled = doLightning;
	}

	//lights
	if(doLightning) {
		if(lights.size()>0) {
			ofSetSmoothLighting(true);
			ofEnableLighting();
		}

		for(Light* light: lights) {
			light->begin();
		}
	}

	//action

	//draw other object types in debug
	if(doDebug) {
		for(Object* obj: objects) {
			if(obj->type != MESH) {
				obj->draw(this, false);
			}
		}
	}

	//collect meshes
	std::vector<Mesh*> meshesNoTransp;
	std::vector<Mesh*> meshesTransp;
	for(Mesh* mesh:meshes) {
		if(mesh->isTransparent)
			meshesTransp.push_back(mesh);
		else
			meshesNoTransp.push_back(mesh);
	}

	//sort transparent meshes by zIndex
	std::sort(meshesTransp.begin(), meshesTransp.end(), sortByDistToCamera(camera->getGlobalPosition()));

	//draw meshes
	for(Mesh* mesh:meshesNoTransp) {
		mesh->draw(this, false);
	}
	
	//draw meshes
	for(Mesh* mesh:meshesTransp) {
		mesh->draw(this, false);
	}

	//
	glDisable(GL_CULL_FACE);
	glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);

	//kill the lights
	if(doLightning) {
		for(Light* light: lights) {
			light->end();
		}
	}

	ofDisableDepthTest();
	ofDisableLighting();
	ofDisableNormalizedTexCoords();

	//draw object names
	if(doDebug) {
		ofSetColor(255);
		for(Object* obj: objects) {
			ofDrawBitmapString(obj->name, obj->getGlobalPosition());
		}
	}

	ofPopStyle();

	//end the camera
	camera->end();


}

void Scene::addObject(Object* obj) {
	if(std::find(objects.begin(), objects.end(), obj) != objects.end()) {
		return;
	}

	objects.push_back(obj);
	timeline.add(&obj->timeline);

	switch(obj->type) {
	case MESH:
		meshes.push_back(static_cast<Mesh*>(obj));

		for(Material* material: meshes.back()->materials) {
			if(material && std::find(materials.begin(), materials.end(), material)==materials.end())
				materials.push_back(material);
		}

		break;
	case CAMERA:
		cameras.push_back(static_cast<Camera*>(obj));
		break;
	case LIGHT:
		lights.push_back(static_cast<Light*>(obj));
		break;
	default:
		break;
	}

	obj->scene = this;

	ofLogNotice(OFX_BLENDER) << "Added object " << obj->name << " to scene " << name;
}

//templated helper to retrieve objects
template<typename Type>
Type* getFromVecByName(std::vector<Type*> vec, string name) {
	for(Type* obj: vec) {
		if(obj->name == name)
			return obj;
	}
	return NULL;
}

template<typename Type>
Type* getFromVecByIndex(std::vector<Type*> vec, unsigned int index) {
	if(vec.size() > index) {
		return vec[index];
	}
	return NULL;
}

bool Scene::hasObject(Object* obj) {
	return std::find(objects.begin(), objects.end(), obj)!=objects.end();
}

Object* Scene::getObject(string name) {
	return getFromVecByName<Object>(objects, name);
}

Object* Scene::getObject(unsigned int index) {
	return getFromVecByIndex<Object>(objects, index);
}

Mesh* Scene::getMesh(string name) {
	return getFromVecByName<Mesh>(meshes, name);
}

Mesh* Scene::getMesh(unsigned int index) {
	return getFromVecByIndex<Mesh>(meshes, index);
}

std::vector<Mesh*> Scene::getMeshes() {
	return meshes;
}

Camera* Scene::getCamera(string name) {
	return getFromVecByName<Camera>(cameras, name);
}

Camera* Scene::getCamera(unsigned int index) {
	return getFromVecByIndex<Camera>(cameras, index);
}

void Scene::setActiveCamera(Camera* cam) {
	addObject(cam);
	activeCamera = cam;
}

void Scene::disableCamera() {
	setActiveCamera(NULL);
}

Camera* Scene::getActiveCamera() {
	return activeCamera;
}

ofCamera* Scene::getDebugCamera() {
	return &debugCam;
}

Light* Scene::getLight(string name) {
	return getFromVecByName<Light>(lights, name);
}

Light* Scene::getLight(unsigned int index) {
	return getFromVecByIndex<Light>(lights, index);
}

void Scene::setLightningEnabled(bool state) {
	doLightning = state;
}

//viewport
void Scene::setViewport(float x, float y, float w, float h) {
	bHasViewport = true;
	viewport.set(x, y, w, h);
	if(activeCamera)
		activeCamera->updateLens();
}

ofRectangle& Scene::getViewport() {
	return viewport;
}

bool Scene::hasViewport() {
	return bHasViewport;
}

void Scene::enableAlphaOrdering() {

}

}
} //end namespace
