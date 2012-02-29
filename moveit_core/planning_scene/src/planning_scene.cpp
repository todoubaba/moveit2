/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2011, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan */

#include "planning_scene/planning_scene.h"
#include <collision_detection/fcl/collision_world.h>
#include <collision_detection/fcl/collision_robot.h>
#include <geometric_shapes/shape_operations.h>
#include <planning_models/conversions.h>

namespace planning_scene
{
typedef collision_detection::CollisionWorldFCL DefaultCWorldType;
typedef collision_detection::CollisionRobotFCL DefaultCRobotType;
static const std::string COLLISION_MAP_NS = "__map";
static const std::string DEFAULT_SCENE_NAME = "(noname)";
}

planning_scene::PlanningScenePtr planning_scene::clone(const PlanningSceneConstPtr &scene)
{
  PlanningScenePtr result(new PlanningScene(scene));
  result->decoupleParent();
  return result;
}

planning_scene::PlanningScene::PlanningScene(void) : configured_(false)
{
  name_ = DEFAULT_SCENE_NAME;
}

planning_scene::PlanningScene::PlanningScene(const PlanningSceneConstPtr &parent) : parent_(parent), configured_(false)
{
  if (parent_)
  {
    if (parent_->isConfigured())
      configure(parent_->getUrdfModel(), parent_->getSrdfModel());
    if (!parent_->getName().empty())
      name_ = parent_->getName() + "+";
  }
  else
  {
    ROS_ERROR("NULL parent scene specified. Ignoring.");
    name_ = DEFAULT_SCENE_NAME;
  }
}

bool planning_scene::PlanningScene::configure(const boost::shared_ptr<const urdf::Model> &urdf_model,
                                              const boost::shared_ptr<const srdf::Model> &srdf_model)
{
  if (!parent_)
  {
    urdf_model_ = urdf_model;
    srdf_model_ = srdf_model;

    kmodel_.reset(new planning_models::KinematicModel(urdf_model, srdf_model));
    kmodel_const_ = kmodel_;
    ftf_.reset(new planning_models::Transforms(kmodel_->getModelFrame()));
    ftf_const_ = ftf_;

    kstate_.reset(new planning_models::KinematicState(kmodel_));
    kstate_->setToDefaultValues();
    acm_.reset(new collision_detection::AllowedCollisionMatrix());

    crobot_.reset(new DefaultCRobotType(kmodel_));
    crobot_unpadded_.reset(new DefaultCRobotType(kmodel_));
    crobot_const_ = crobot_;
    crobot_unpadded_const_ = crobot_unpadded_;

    cworld_.reset(new DefaultCWorldType());
    cworld_const_ = cworld_;

    configured_ = true;
  }
  else
  {
    if (parent_->isConfigured())
    {
      if (srdf_model != parent_->getSrdfModel() || urdf_model != parent_->getUrdfModel())
        ROS_ERROR("Parent of planning scene is not constructed from the same robot models");

      // even if we have a parent, we do maintain a separate world representation, one that records changes
      // this is cheap however, because the worlds share the world representation
      cworld_.reset(new DefaultCWorldType(static_cast<const DefaultCWorldType&>(*parent_->getCollisionWorld())));
      cworld_->recordChanges(true);
      cworld_const_ = cworld_;
      configured_ = true;
    }
    else
      ROS_ERROR("Parent is not configured yet");
  }

  return configured_;
}

void planning_scene::PlanningScene::clearDiffs(void)
{
  if (!parent_)
    return;

  // clear everything, reset the world
  cworld_.reset(new DefaultCWorldType(static_cast<const DefaultCWorldType&>(*parent_->getCollisionWorld())));
  cworld_->recordChanges(true);
  cworld_const_ = cworld_;

  kmodel_.reset();
  kmodel_const_.reset();
  ftf_.reset();
  ftf_const_.reset();
  kstate_.reset();
  acm_.reset();
  crobot_.reset();
  crobot_const_.reset();
  crobot_unpadded_.reset();
  crobot_unpadded_const_.reset();
}

void planning_scene::PlanningScene::pushDiffs(const PlanningScenePtr &scene)
{
  if (!parent_)
    return;

  if (ftf_)
    *scene->getTransforms() = *ftf_;

  if (kstate_)
    scene->getCurrentState() = *kstate_;

  if (acm_)
    scene->getAllowedCollisionMatrix() = *acm_;

  if (crobot_)
  {
    scene->getCollisionRobot()->setLinkPadding(crobot_->getLinkPadding());
    scene->getCollisionRobot()->setLinkScale(crobot_->getLinkScale());
  }

  if (cworld_->isRecordingChanges())
  {
    const std::vector<collision_detection::CollisionWorld::Change> &changes = cworld_->getChanges();
    if (!changes.empty())
    {
      collision_detection::CollisionWorldPtr w = scene->getCollisionWorld();
      for (std::size_t i = 0 ; i < changes.size() ; ++i)
        if (changes[i].type_ == collision_detection::CollisionWorld::Change::ADD)
        {
          collision_detection::CollisionWorld::Object *obj = cworld_->getObject(changes[i].id_)->clone();
          w->addToObject(obj->id_, obj->shapes_, obj->shape_poses_);
          w->addToObject(obj->id_, obj->static_shapes_);
          // memory now belongs to the other collision world, so we do not delete it
          obj->shapes_.clear(); obj->shape_poses_.clear(); obj->static_shapes_.clear();
          delete obj;
        }
        else
          if (changes[i].type_ == collision_detection::CollisionWorld::Change::REMOVE)
            w->removeObject(changes[i].id_);
          else
            ROS_ERROR("Unknown change on collision world");
    }
  }
}

void planning_scene::PlanningScene::checkCollision(const collision_detection::CollisionRequest& req, collision_detection::CollisionResult &res) const
{
  checkCollision(req, res, getCurrentState());
}

void planning_scene::PlanningScene::checkSelfCollision(const collision_detection::CollisionRequest& req, collision_detection::CollisionResult &res) const
{
  checkSelfCollision(req, res, getCurrentState());
}

void planning_scene::PlanningScene::checkCollision(const collision_detection::CollisionRequest& req, collision_detection::CollisionResult &res,
                                                   const planning_models::KinematicState &kstate) const
{
  // check collision with the world using the padded version
  if (parent_)
    getCollisionWorld()->checkRobotCollision(req, res, *getCollisionRobot(), kstate, getAllowedCollisionMatrix());
  else
    getCollisionWorld()->checkRobotCollision(req, res, *crobot_, kstate, *acm_);
  
  if (!res.collision || (req.contacts && res.contacts.size() < req.max_contacts))
  {
    // do self-collision checking with the unpadded version of the robot
    if (parent_)
      getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, getAllowedCollisionMatrix());
    else
      getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, *acm_);
  }
}

void planning_scene::PlanningScene::checkSelfCollision(const collision_detection::CollisionRequest& req, collision_detection::CollisionResult &res,
                                                       const planning_models::KinematicState &kstate) const
{
  // do self-collision checking with the unpadded version of the robot
  getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, getAllowedCollisionMatrix());
}

void planning_scene::PlanningScene::checkCollision(const collision_detection::CollisionRequest& req,
                                                   collision_detection::CollisionResult &res,
                                                   const planning_models::KinematicState &kstate,
                                                   const collision_detection::AllowedCollisionMatrix& acm) const
{
  // check collision with the world using the padded version
  getCollisionWorld()->checkRobotCollision(req, res, *getCollisionRobot(), kstate, acm);
  
  // do self-collision checking with the unpadded version of the robot
  if (!res.collision || (req.contacts && res.contacts.size() < req.max_contacts))
  {
    getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, acm);
  }
}

void planning_scene::PlanningScene::checkCollisionUnpadded(const collision_detection::CollisionRequest& req,
                                                           collision_detection::CollisionResult &res,
                                                           const planning_models::KinematicState &kstate,
                                                           const collision_detection::AllowedCollisionMatrix& acm) const
{
  // check collision with the world using the padded version
  getCollisionWorld()->checkRobotCollision(req, res, *getCollisionRobotUnpadded(), kstate, acm);

  // do self-collision checking with the unpadded version of the robot
  if (!res.collision || (req.contacts && res.contacts.size() < req.max_contacts))
  {
    getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, acm);
  }
}

void planning_scene::PlanningScene::checkSelfCollision(const collision_detection::CollisionRequest& req,
                                                       collision_detection::CollisionResult &res,
                                                       const planning_models::KinematicState &kstate,
                                                       const collision_detection::AllowedCollisionMatrix& acm) const
{
  // do self-collision checking with the unpadded version of the robot
  getCollisionRobotUnpadded()->checkSelfCollision(req, res, kstate, acm);
}

const collision_detection::CollisionRobotPtr& planning_scene::PlanningScene::getCollisionRobot(void)
{
  if (!crobot_)
  {
    crobot_.reset(new DefaultCRobotType(static_cast<const DefaultCRobotType&>(*parent_->getCollisionRobot())));
    crobot_const_ = crobot_;
  }
  return crobot_;
}

planning_models::KinematicState& planning_scene::PlanningScene::getCurrentState(void)
{
  if (!kstate_)
    kstate_.reset(new planning_models::KinematicState(parent_->getCurrentState()));
  return *kstate_;
}

collision_detection::AllowedCollisionMatrix& planning_scene::PlanningScene::getAllowedCollisionMatrix(void)
{
  if (!acm_)
    acm_.reset(new collision_detection::AllowedCollisionMatrix(parent_->getAllowedCollisionMatrix()));
  return *acm_;
}

const planning_models::TransformsPtr& planning_scene::PlanningScene::getTransforms(void)
{
  if (!ftf_)
  {
    ftf_.reset(new planning_models::Transforms(*parent_->getTransforms()));
    ftf_const_ = ftf_;
  }
  return ftf_;
}

void planning_scene::PlanningScene::getPlanningSceneDiffMsg(moveit_msgs::PlanningScene &scene) const
{
  scene.name = name_;
  if (ftf_)
    ftf_->getTransforms(scene.fixed_frame_transforms);
  else
    scene.fixed_frame_transforms.clear();

  if (kstate_)
  {
    planning_models::kinematicStateToRobotState(*kstate_, scene.robot_state);
    getPlanningSceneMsgAttachedBodies(scene);
  }
  else
  {
    scene.robot_state = moveit_msgs::RobotState();
    scene.attached_collision_objects.clear();
  }

  if (acm_)
    acm_->getMessage(scene.allowed_collision_matrix);
  else
    scene.allowed_collision_matrix = moveit_msgs::AllowedCollisionMatrix();

  if (crobot_)
  {
    crobot_->getPadding(scene.link_padding);
    crobot_->getScale(scene.link_scale);
  }
  else
  {
    scene.link_padding.clear();
    scene.link_scale.clear();
  }

  if (cworld_->isRecordingChanges())
  {
    scene.world.collision_objects.clear();
    scene.world.collision_map = moveit_msgs::CollisionMap();

    bool skip_cmap = false;
    const std::vector<collision_detection::CollisionWorld::Change> &changes = cworld_->getChanges();
    for (std::size_t i = 0 ; i < changes.size() ; ++i)
      if (changes[i].id_ == COLLISION_MAP_NS)
      {
        if (!skip_cmap)
        {
          skip_cmap = true;
          getPlanningSceneMsgCollisionMap(scene);
        }
      }
      else
      {
        if (changes[i].type_ == collision_detection::CollisionWorld::Change::ADD)
        {
          addPlanningSceneMsgCollisionObject(scene, changes[i].id_);
        }
        else
          if (changes[i].type_ == collision_detection::CollisionWorld::Change::REMOVE)
          {
            moveit_msgs::CollisionObject co;
            co.header.frame_id = getPlanningFrame();
            co.id = changes[i].id_;
            co.operation = moveit_msgs::CollisionObject::REMOVE;
            scene.world.collision_objects.push_back(co);
          }
          else
            ROS_ERROR("Unknown change on collision world");
      }
  }
  else
  {
    getPlanningSceneMsgCollisionObjects(scene);
    getPlanningSceneMsgCollisionMap(scene);
  }
}

void planning_scene::PlanningScene::getPlanningSceneMsgAttachedBodies(moveit_msgs::PlanningScene &scene) const
{
  scene.attached_collision_objects.clear();
  std::vector<const planning_models::KinematicState::AttachedBody*> ab;
  getCurrentState().getAttachedBodies(ab);

  for (std::size_t i = 0 ; i < ab.size() ; ++i)
  {
    moveit_msgs::AttachedCollisionObject aco;
    aco.link_name = ab[i]->getAttachedLinkName();
    const std::set<std::string> &touch_links = ab[i]->getTouchLinks();
    for (std::set<std::string>::const_iterator it = touch_links.begin() ; it != touch_links.end() ; ++it)
      aco.touch_links.push_back(*it);
    aco.object.header.frame_id = aco.link_name;
    aco.object.id = ab[i]->getName();
    aco.object.operation = moveit_msgs::CollisionObject::ADD;
    const std::vector<shapes::Shape*>& ab_shapes = ab[i]->getShapes();
    const std::vector<Eigen::Affine3d>& ab_tf = ab[i]->getFixedTransforms();
    for (std::size_t j = 0 ; j < ab_shapes.size() ; ++j)
    {
      moveit_msgs::Shape sm;
      if (constructMsgFromShape(ab_shapes[j], sm))
      {
        aco.object.shapes.push_back(sm);
        geometry_msgs::Pose p;
        planning_models::msgFromPose(ab_tf[j], p);
        aco.object.poses.push_back(p);
      }
    }
    if (!aco.object.shapes.empty())
      scene.attached_collision_objects.push_back(aco);
  }
}

void planning_scene::PlanningScene::addPlanningSceneMsgCollisionObject(moveit_msgs::PlanningScene &scene, const std::string &ns) const
{
  moveit_msgs::CollisionObject co;
  co.header.frame_id = getPlanningFrame();
  co.id = ns;
  co.operation = moveit_msgs::CollisionObject::ADD;
  const collision_detection::CollisionWorld::Object &obj = *getCollisionWorld()->getObject(ns);
  for (std::size_t j = 0 ; j < obj.static_shapes_.size() ; ++j)
  {
    moveit_msgs::StaticShape sm;
    if (constructMsgFromShape(obj.static_shapes_[j], sm))
      co.static_shapes.push_back(sm);
  }
  for (std::size_t j = 0 ; j < obj.shapes_.size() ; ++j)
  {
    moveit_msgs::Shape sm;
    if (constructMsgFromShape(obj.shapes_[j], sm))
    {
      co.shapes.push_back(sm);
      geometry_msgs::Pose p;
      planning_models::msgFromPose(obj.shape_poses_[j], p);
      co.poses.push_back(p);
    }
  }
  if (!co.shapes.empty() || !co.static_shapes.empty())
    scene.world.collision_objects.push_back(co);
}

void planning_scene::PlanningScene::getPlanningSceneMsgCollisionObjects(moveit_msgs::PlanningScene &scene) const
{
  scene.world.collision_objects.clear();
  const std::vector<std::string> &ns = getCollisionWorld()->getObjectIds();
  for (std::size_t i = 0 ; i < ns.size() ; ++i)
    if (ns[i] != COLLISION_MAP_NS)
      addPlanningSceneMsgCollisionObject(scene, ns[i]);
}

void planning_scene::PlanningScene::getPlanningSceneMsgCollisionMap(moveit_msgs::PlanningScene &scene) const
{
  scene.world.collision_map.header.frame_id = getPlanningFrame();
  scene.world.collision_map.boxes.clear();
  if (getCollisionWorld()->hasObject(COLLISION_MAP_NS))
  {
    const collision_detection::CollisionWorld::Object& map = *getCollisionWorld()->getObject(COLLISION_MAP_NS);
    if (!map.static_shapes_.empty())
      ROS_ERROR("Static shapes are not supported in the collision map.");
    for (std::size_t i = 0 ; i < map.shapes_.size() ; ++i)
    {
      shapes::Box *b = static_cast<shapes::Box*>(map.shapes_[i]);
      moveit_msgs::OrientedBoundingBox obb;
      obb.extents.x = b->size[0]; obb.extents.y = b->size[1]; obb.extents.z = b->size[2];
      planning_models::msgFromPose(map.shape_poses_[i], obb.pose);
      scene.world.collision_map.boxes.push_back(obb);
    }
  }
}

void planning_scene::PlanningScene::getPlanningSceneMsg(moveit_msgs::PlanningScene &scene) const
{
  scene.name = name_;
  getTransforms()->getTransforms(scene.fixed_frame_transforms);
  planning_models::kinematicStateToRobotState(getCurrentState(), scene.robot_state);
  getAllowedCollisionMatrix().getMessage(scene.allowed_collision_matrix);
  getCollisionRobot()->getPadding(scene.link_padding);
  getCollisionRobot()->getScale(scene.link_scale);

  // add collision objects
  getPlanningSceneMsgCollisionObjects(scene);

  // add the attached bodies
  getPlanningSceneMsgAttachedBodies(scene);

  // get the collision map
  getPlanningSceneMsgCollisionMap(scene);
}

void planning_scene::PlanningScene::setCurrentState(const moveit_msgs::RobotState &state)
{
  if (parent_)
  {
    if (!kstate_)
      kstate_.reset(new planning_models::KinematicState(parent_->getCurrentState()));
    planning_models::robotStateToKinematicState(*getTransforms(), state, *kstate_);
  }
  else
    planning_models::robotStateToKinematicState(*ftf_, state, *kstate_);
}

void planning_scene::PlanningScene::setCurrentState(const planning_models::KinematicState &state)
{
  if (!kstate_)
    kstate_.reset(new planning_models::KinematicState(getKinematicModel()));
  *kstate_ = state;
}

void planning_scene::PlanningScene::decoupleParent(void)
{
  if (!parent_)
    return;
  if (parent_->isConfigured())
  {
    urdf_model_ = parent_->urdf_model_;
    srdf_model_ = parent_->srdf_model_;
    kmodel_ = parent_->kmodel_;
    kmodel_const_ = kmodel_;

    if (!ftf_)
    {
      ftf_.reset(new planning_models::Transforms(*parent_->getTransforms()));
      ftf_const_ = ftf_;
    }

    if (!kstate_)
      kstate_.reset(new planning_models::KinematicState(parent_->getCurrentState()));

    if (!acm_)
      acm_.reset(new collision_detection::AllowedCollisionMatrix(parent_->getAllowedCollisionMatrix()));

    if(!crobot_unpadded_) {
      crobot_unpadded_.reset(new DefaultCRobotType(static_cast<const DefaultCRobotType&>(*parent_->getCollisionRobotUnpadded())));
      crobot_unpadded_const_ = crobot_unpadded_;
    }
    if (!crobot_)
    {
      crobot_.reset(new DefaultCRobotType(static_cast<const DefaultCRobotType&>(*parent_->getCollisionRobot())));
      crobot_const_ = crobot_;
    }

    if (!cworld_)
    {
      cworld_.reset(new DefaultCWorldType(static_cast<const DefaultCWorldType&>(*parent_->getCollisionWorld())));
      cworld_const_ = cworld_;
    }
    else
    {
      cworld_->recordChanges(false);
      cworld_->clearChanges();
    }

    configured_ = true;
  }

  parent_.reset();
}

void planning_scene::PlanningScene::setPlanningSceneDiffMsg(const moveit_msgs::PlanningScene &scene)
{
  ROS_DEBUG("Adding planning scene diff");
  name_ = scene.name;

  // there is at least one transform in the list of fixed transform: from model frame to itself;
  // if the list is empty, then nothing has been set
  if (!scene.fixed_frame_transforms.empty())
  {
    if (!ftf_)
    {
      ftf_.reset(new planning_models::Transforms(getKinematicModel()->getModelFrame()));
      ftf_const_ = ftf_;
    }
    ftf_->setTransforms(scene.fixed_frame_transforms);
  }

  // if at least some joints have been specified, we set them
  if (!scene.robot_state.multi_dof_joint_state.joint_names.empty() ||
      !scene.robot_state.joint_state.name.empty())
    setCurrentState(scene.robot_state);

  if (!scene.attached_collision_objects.empty())
    for (std::size_t i = 0 ; i < scene.attached_collision_objects.size() ; ++i)
      processAttachedCollisionObjectMsg(scene.attached_collision_objects[i]);

  // if at least some links are mentioned in the allowed collision matrix, then we have an update
  if (!scene.allowed_collision_matrix.entry_names.empty())
    acm_.reset(new collision_detection::AllowedCollisionMatrix(scene.allowed_collision_matrix));

  if (!scene.link_padding.empty() || !scene.link_scale.empty())
  {
    if (!crobot_)
    { // this means we have a parent too
      crobot_.reset(new DefaultCRobotType(static_cast<const DefaultCRobotType&>(*parent_->getCollisionRobot())));
      crobot_const_ = crobot_;
    }
    crobot_->setPadding(scene.link_padding);
    crobot_->setScale(scene.link_scale);
  }

  if ((!scene.world.collision_map.header.frame_id.empty() &&
       !scene.world.collision_map.boxes.empty()) || !scene.world.collision_objects.empty())
  {
    for (std::size_t i = 0 ; i < scene.world.collision_objects.size() ; ++i)
      processCollisionObjectMsg(scene.world.collision_objects[i]);

    if (!scene.world.collision_map.header.frame_id.empty() && !scene.world.collision_map.boxes.empty())
      processCollisionMapMsg(scene.world.collision_map);
  }
}

void planning_scene::PlanningScene::setPlanningSceneMsg(const moveit_msgs::PlanningScene &scene)
{
  ROS_DEBUG("Setting new planning scene");
  name_ = scene.name;

  if (parent_)
  {
    // if we have a parent, but we set a new planning scene, then we do not care about the parent any more
    // and we no longer represent the scene as a diff
    urdf_model_ = parent_->urdf_model_;
    srdf_model_ = parent_->srdf_model_;
    kmodel_ = parent_->kmodel_;
    kmodel_const_ = kmodel_;

    if (!ftf_)
    {
      ftf_.reset(new planning_models::Transforms(kmodel_->getModelFrame()));
      ftf_const_ = ftf_;
    }

    if (!kstate_)
      kstate_.reset(new planning_models::KinematicState(kmodel_));

    if (!crobot_)
    {
      crobot_.reset(new DefaultCRobotType(kmodel_));
      crobot_const_ = crobot_;
    }
    crobot_unpadded_.reset(new DefaultCRobotType(kmodel_));
    crobot_unpadded_const_ = crobot_unpadded_;

    cworld_->recordChanges(false);
    cworld_->clearChanges();

    configured_ = true;
    parent_.reset();
  }
  ftf_->setTransforms(scene.fixed_frame_transforms);
  setCurrentState(scene.robot_state);
  acm_.reset(new collision_detection::AllowedCollisionMatrix(scene.allowed_collision_matrix));
  crobot_->setPadding(scene.link_padding);
  crobot_->setScale(scene.link_scale);
  cworld_->clearObjects();
  for (std::size_t i = 0 ; i < scene.world.collision_objects.size() ; ++i)
    processCollisionObjectMsg(scene.world.collision_objects[i]);
  kstate_->clearAttachedBodies();
  for (std::size_t i = 0 ; i < scene.attached_collision_objects.size() ; ++i)
    processAttachedCollisionObjectMsg(scene.attached_collision_objects[i]);
  processCollisionMapMsg(scene.world.collision_map);
}

void planning_scene::PlanningScene::processCollisionMapMsg(const moveit_msgs::CollisionMap &map)
{
  const Eigen::Affine3d &t = getTransforms()->getTransform(getCurrentState(), map.header.frame_id);
  for (std::size_t i = 0 ; i < map.boxes.size() ; ++i)
  {
    Eigen::Affine3d p; planning_models::poseFromMsg(map.boxes[i].pose, p);
    shapes::Shape *s = new shapes::Box(map.boxes[i].extents.x, map.boxes[i].extents.y, map.boxes[i].extents.z);
    cworld_->addToObject(COLLISION_MAP_NS, s, t * p);
  }
}

bool planning_scene::PlanningScene::processAttachedCollisionObjectMsg(const moveit_msgs::AttachedCollisionObject &object)
{
  if (!getKinematicModel()->hasLinkModel(object.link_name))
  {
    ROS_ERROR("Unable to attach a body to link '%s' (link not found)", object.link_name.c_str());
    return false;
  }

  if (object.object.id == COLLISION_MAP_NS)
  {
    ROS_ERROR("The ID '%s' cannot be used for collision objects (name reserved)", COLLISION_MAP_NS.c_str());
    return false;
  }

  if (!kstate_) // there must be a parent in this case
    kstate_.reset(new planning_models::KinematicState(parent_->getCurrentState()));

  if (object.object.operation == moveit_msgs::CollisionObject::ADD)
  {
    if (object.object.shapes.size() != object.object.poses.size())
    {
      ROS_ERROR("Number of shapes does not match number of poses in attached collision object message");
      return false;
    }

    planning_models::KinematicState::LinkState *ls = kstate_->getLinkState(object.link_name);
    if (ls)
    {
      std::vector<shapes::Shape*> shapes;
      std::vector<Eigen::Affine3d> poses;

      // we need to add some shapes; if the message is empty, maybe the object is already in the world
      if (object.object.shapes.empty())
      {
        if (cworld_->hasObject(object.object.id))
        {
          ROS_DEBUG("Attaching world object '%s' to link '%s'", object.object.id.c_str(), object.link_name.c_str());

          // extract the shapes from the world
          collision_detection::CollisionWorld::ObjectPtr obj = cworld_->getObject(object.object.id);
          shapes = obj->shapes_;
          poses = obj->shape_poses_;
          // remove the pointer to the objects from the collision world
          cworld_->removeObject(object.object.id);

          if (obj.unique())
          {
            // make sure the memory for the shapes is not deleted
            obj->shapes_.clear();
            ROS_DEBUG("The memory representing shapes was moved from the collision world to the planning model");
          }
          else
          {
            // clone the shapes because we cannot assume their ownership; this will probably rarely happen (if ever)
            for (std::size_t i = 0 ; i < shapes.size() ; ++i)
              shapes[i] = shapes[i]->clone();
            ROS_DEBUG("The memory representing shapes was copied from the collision world to the planning model");
          }

          if (!obj->static_shapes_.empty())
            ROS_WARN("Static shapes from object '%s' are lost when the object is attached to the robot", object.object.id.c_str());

          // need to transform poses to the link frame
          const Eigen::Affine3d &i_t = ls->getGlobalLinkTransform().inverse();
          for (std::size_t i = 0 ; i < poses.size() ; ++i)
            poses[i] = i_t * poses[i];
        }
        else
        {
          ROS_ERROR("Attempting to attach object '%s' to link '%s' but no geometry specified and such an object does not exist in the collision world",
                    object.object.id.c_str(), object.link_name.c_str());
          return false;
        }
      }
      else
      {
        // we clear the world objects with the same name, since we got an update on their geometry
        if (cworld_->hasObject(object.object.id))
          cworld_->removeObject(object.object.id);
        if (!object.object.static_shapes.empty())
          ROS_ERROR("Static shapes are ignored for attached object '%s'", object.object.id.c_str());

        for (std::size_t i = 0 ; i < object.object.shapes.size() ; ++i)
        {
          shapes::Shape *s = shapes::constructShapeFromMsg(object.object.shapes[i]);
          if (s)
          {
            Eigen::Affine3d p; planning_models::poseFromMsg(object.object.poses[i], p);
            shapes.push_back(s);
            poses.push_back(p);
          }
        }
        // transform poses to link frame
        if (object.object.header.frame_id != object.link_name)
        {
          const Eigen::Affine3d &t = ls->getGlobalLinkTransform().inverse() * getTransforms()->getTransform(*kstate_, object.object.header.frame_id);
          for (std::size_t i = 0 ; i < poses.size() ; ++i)
            poses[i] = t * poses[i];
        }
      }

      if (shapes.empty())
      {
        ROS_ERROR("There is no geometry to attach to link '%s' as part of attached body '%s'", object.link_name.c_str(), object.object.id.c_str());
        return false;
      }

      // there should not exist an attached object with this name
      if (ls->clearAttachedBody(object.object.id))
        ROS_WARN("The kinematic state already had an object named '%s' attached to link '%s'. The object was replaced.",
                 object.object.id.c_str(), object.link_name.c_str());
      ls->attachBody(object.object.id, shapes, poses, object.touch_links);
      ROS_DEBUG("Attached object '%s' to link '%s'", object.object.id.c_str(), object.link_name.c_str());
      return true;
    }
    else
      ROS_FATAL("Kinematic state is not compatible with kinematic model");
  }
  else
    if (object.object.operation == moveit_msgs::CollisionObject::REMOVE)
    {
      planning_models::KinematicState::LinkState *ls = kstate_->getLinkState(object.link_name);
      if (ls)
      {
        const planning_models::KinematicState::AttachedBody *ab = ls->getAttachedBody(object.object.id);
        if (ab)
        {
          boost::shared_ptr<planning_models::KinematicState::AttachedBodyProperties> prop = ab->getProperties();
          std::vector<Eigen::Affine3d> poses = ab->getGlobalCollisionBodyTransforms();
          ls->clearAttachedBody(object.object.id);

          if (prop.unique())
          {
            ROS_DEBUG("The memory representing shapes was moved from the planning model to the collision world");
            cworld_->addToObject(object.object.id, prop->shapes_, poses);
            prop->shapes_.clear(); // memory is now owned by the collision world
          }
          else
          {
            // the attached body is used elsewhere, so we do not modify it
            std::vector<shapes::Shape*> shapes(prop->shapes_.size());
            for (std::size_t i = 0 ; i < shapes.size() ; ++i)
              shapes[i] = prop->shapes_[i]->clone();
            ROS_DEBUG("The memory representing shapes was copied from the planning model to the collision world");
            cworld_->addToObject(object.object.id, shapes, poses);
          }
          ROS_DEBUG("Detached object '%s' from link '%s' and added it back in the collision world", object.object.id.c_str(), object.link_name.c_str());
          return true;
        }
        else
          ROS_ERROR("No object named '%s' is attached to link '%s'", object.object.id.c_str(), object.link_name.c_str());
      }
      else
        ROS_FATAL("Kinematic state is not compatible with kinematic model");
    }
    else
      ROS_ERROR("Unknown collision object operation: %d", object.object.operation);
  return false;
}

bool planning_scene::PlanningScene::processCollisionObjectMsg(const moveit_msgs::CollisionObject &object)
{
  if (object.id == COLLISION_MAP_NS)
  {
    ROS_ERROR("The ID '%s' cannot be used for collision objects (name reserved)", COLLISION_MAP_NS.c_str());
    return false;
  }
  //  collision_detection::AllowedCollisionMatrix& acm = getAllowedCollisionMatrix();

  if (object.operation == moveit_msgs::CollisionObject::ADD)
  {
    if (object.shapes.empty() && object.static_shapes.empty())
    {
      ROS_ERROR("There are no shapes specified in the collision object message");
      return false;
    }
    if (object.shapes.size() != object.poses.size())
    {
      ROS_ERROR("Number of shapes does not match number of poses in collision object message");
      return false;
    }

    for (std::size_t i = 0 ; i < object.static_shapes.size() ; ++i)
    {
      shapes::StaticShape *s = shapes::constructShapeFromMsg(object.static_shapes[i]);
      if (s)
        cworld_->addToObject(object.id, s);
    }

    const Eigen::Affine3d &t = getTransforms()->getTransform(getCurrentState(), object.header.frame_id);
    for (std::size_t i = 0 ; i < object.shapes.size() ; ++i)
    {
      shapes::Shape *s = shapes::constructShapeFromMsg(object.shapes[i]);
      if (s)
      {
        Eigen::Affine3d p; planning_models::poseFromMsg(object.poses[i], p);
        cworld_->addToObject(object.id, s, t * p);
      }
    }
    /*
    if(!acm.hasEntry(object.id))
    {
      ROS_INFO_STREAM("Adding entry for " << object.id);
      acm.setEntry(object.id, false);
    }
    */
    return true;
  }
  else
    if (object.operation == moveit_msgs::CollisionObject::REMOVE)
    {
      cworld_->removeObject(object.id);
      /*
      if(acm.hasEntry(object.id))
      {
        ROS_INFO_STREAM("Removing entry for " << object.id);
        acm.removeEntry(object.id);
	}*/
      return true;
    }
    else
      ROS_ERROR("Unknown collision object operation: %d", object.operation);
  return false;
}

bool planning_scene::PlanningScene::isPathValid(const moveit_msgs::RobotState &start_state, 
                                                const moveit_msgs::RobotTrajectory &trajectory) const
{
  planning_models::KinematicState start(getCurrentState());
  planning_models::robotStateToKinematicState(*getTransforms(), start_state, start);
  moveit_msgs::Constraints emp_constraints;
  return isPathValid(&start, emp_constraints, emp_constraints, trajectory);
}

bool planning_scene::PlanningScene::isPathValid(const planning_models::KinematicState* state,
                                                const moveit_msgs::Constraints& path_constraints,
                                                const moveit_msgs::Constraints& goal_constraints,
                                                const moveit_msgs::RobotTrajectory &trajectory) const
{
  //TODO - check path and goal constraints
  planning_models::KinematicState start(*state);
  std::size_t state_count = std::max(trajectory.joint_trajectory.points.size(),
                                     trajectory.multi_dof_joint_trajectory.points.size());
  for (std::size_t i = 0 ; i < state_count ; ++i)
  {
    moveit_msgs::RobotState rs;
    planning_models::robotTrajectoryPointToRobotState(trajectory, i, rs);
    planning_models::KinematicStatePtr st(new planning_models::KinematicState(start));
    planning_models::robotStateToKinematicState(*getTransforms(), rs, *st);
    collision_detection::CollisionRequest req;
    collision_detection::CollisionResult  res;
    checkCollision(req, res, *st);
    if (res.collision)
      return false;
  }
  return true;
} 

void planning_scene::PlanningScene::convertToKinematicStates(const moveit_msgs::RobotState &start_state, const moveit_msgs::RobotTrajectory &trajectory,
							     std::vector<planning_models::KinematicStatePtr> &states) const
{
  states.clear();
  planning_models::KinematicState start(getCurrentState());
  planning_models::robotStateToKinematicState(*getTransforms(), start_state, start);
  std::size_t state_count = std::max(trajectory.joint_trajectory.points.size(),
                                     trajectory.multi_dof_joint_trajectory.points.size());
  states.resize(state_count);
  for (std::size_t i = 0 ; i < state_count ; ++i)
  {
    moveit_msgs::RobotState rs;
    planning_models::robotTrajectoryPointToRobotState(trajectory, i, rs);
    planning_models::KinematicStatePtr st(new planning_models::KinematicState(start));
    planning_models::robotStateToKinematicState(*getTransforms(), rs, *st);
    states[i] = st;
  }
}

collision_detection::AllowedCollisionMatrix planning_scene::PlanningScene::disableCollisionsForNonUpdatedLinks(const std::string& group) const 
{
  collision_detection::AllowedCollisionMatrix acm = getAllowedCollisionMatrix();
  const planning_models::KinematicModel::JointModelGroup* jmg = getKinematicModel()->getJointModelGroup(group);
  if(jmg == NULL) {
    ROS_WARN_STREAM("Can't disable collisions for non-existent group " << group);
    return acm;
  }
  const std::vector<std::string> all_links = getKinematicModel()->getLinkModelNames();
  const std::vector<std::string> updated_links = jmg->getUpdatedLinkModelNames();
  std::map<std::string, bool> updated_link_map;

  for(unsigned int i = 0; i < updated_links.size(); i++) {
    updated_link_map[updated_links[i]] = true;
  }
  //anything not in the map gets set to allowed
  for(unsigned int i = 0; i < all_links.size(); i++) {
    if(updated_link_map.find(all_links[i]) == updated_link_map.end()) {
      acm.setDefaultEntry(all_links[i], true);
    }
  }

  return acm;
}
