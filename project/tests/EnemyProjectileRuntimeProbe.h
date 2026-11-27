#pragma once
#include <cstdint>
#ifdef USE_IMGUI
#include "game/scene/gameplay/GamePlayScene.h"
#include "game/actor/enemy/ranged/EnemyBomber.h"
#include "game/actor/enemy/ranged/EnemyBomb.h"
#include "game/actor/gimmick/block/GimmickBreakableBlock.h"
#include "game/actor/enemy/spawn/EnemyFactory.h"
#include "game/actor/combat/AttackSequence.h"
#include "game/system/combat/BulletManager.h"
#include "tests/GateEntryRuntimeProbe.h"
#include "tests/CannonDamageRuntimeProbe.h"
#include <fstream>
#include <stdexcept>
#include <cstring>

// 明示起動した専用プロセスで、地形接触と個別所有モデルのGPU描画を確認する。
namespace enemyProjectileRuntimeProbe {
inline bool Enabled() { return GetEnvironmentVariableA("CG2_ENEMY_PROJECTILE_TEST",nullptr,0)>0; }
inline void Require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
/// <summary>ボンバーを投擲準備状態に進め、手持ちの爆弾と期待位置を検証コードへ取り出す。</summary>
struct Access {
    static void Hold(EnemyBomber* actor) {
        actor->throwState_=EnemyBomber::ThrowState::kWindup;
        actor->windupTimer_=0.55f;
        actor->throwBombReleased_=false;
        actor->UpdateHeldBombVisual(0);
    }
    static engine::Object3D* Held(EnemyBomber* actor) { return actor->heldBombVisual_.get(); }
    static engine::Vector3 Expected(EnemyBomber* actor) { return actor->ComputeHeldBombPosition(actor->GetThrowProgress()); }
    static void Release(EnemyBomber* actor) {
        actor->ThrowBomb();actor->throwBombReleased_=true;
        actor->throwState_=EnemyBomber::ThrowState::kLanding;actor->throwLandingTimer_=10;
    }
};
inline bool Same(const engine::Matrix4x4& a,const engine::Matrix4x4& b,float tolerance=0.01f) {
    for(std::int32_t i=0;i<4;++i)for(std::int32_t j=0;j<4;++j)if(std::abs(a.m[i][j]-b.m[i][j])>tolerance)return false;
    return true;
}
inline void FireCases(BaseScene* scene,std::ostream& log) {
    auto* bullets=BulletManager::GetInstance();auto* collisions=engine::CollisionManager::GetInstance();bullets->Clear();
    auto floor=std::make_unique<engine::Object3D>();floor->Initialize(scene->GetObject3DCommon());
    floor->SetTranslate({0,1000,10});floor->SetScale({1,1,1});
    engine::Object3D::ColliderConfig box;box.type=engine::ColliderType::kAabb;box.size={8,0.1f,16};box.center={0,0,0};floor->SetColliderConfig(box);
    floor->SetCollisionAttribute(engine::collisionCategories::Get("ground"));floor->SetCollisionMask(~0u);floor->UpdateWorldMatrix(true);collisions->AddObject(floor.get());
    EnemyAttackProfile profile;std::string error;Require(profile.LoadForEnemy("FireSlime",&error),"fire profile load failed");
    const auto* fire=profile.FindAttack("fireball");Require(fire && fire->sequence.enabled,"missing fire sequence");
    for(float step:{1.0f/60.0f,0.25f}) {
        AttackSequence sequence;AttackSequenceContext context;context.position={0,1002,0};context.forward={0,0,1};
        context.hasTarget=true;context.targetPosition={0,999,12};context.targetDistance=12;
        Require(sequence.Start(*fire,context),"fire sequence failed");sequence.Update(0.44f,context);
        Require(bullets->GetBullets().size()==1,"sequence did not emit one fireball");
        Require(bullets->GetBullets().front()->GetVelocity().y<0,"fireball did not aim at ground");
        float elapsed=0;
        while(!bullets->GetBullets().empty() && elapsed<1.5f) { bullets->Update(step);elapsed+=step; }
        Require(bullets->GetBullets().empty(),"fireball remained alive at floor");
        log<<"PASS authored enemy fireball expires on floor step="<<step<<" elapsed="<<elapsed<<std::endl;
    }
    // 無害な比較表示と大砲の地形非衝突は、今回の火球指定に巻き込まない。
    BulletVisualConfig preview;preview.previewOnly=true;preview.destroyOnSolidCastHit=true;
    bullets->Fire({0,1002,0},{0,-10,0},engine::collisionCategories::Get("enemy_attack"),engine::collisionCategories::Get("ground"),"Primitives/sphere",0.3f,3,preview);
    bullets->Update(0.4f);Require(bullets->GetBullets().size()==1,"preview collided with ground");bullets->Clear();
    auto disabled=*fire;
    for(auto& event:disabled.sequence.events)if(event.operation=="projectile")event.projectile.collisionEnabled=false;
    AttackSequence disabledSequence;AttackSequenceContext context;context.position={0,1002,0};context.forward={0,0,1};context.hasTarget=true;context.targetPosition={0,999,12};context.targetDistance=12;
    Require(disabledSequence.Start(disabled,context),"disabled fire sequence failed");disabledSequence.Update(0.44f,context);bullets->Update(1.0f);
    Require(bullets->GetBullets().size()==1,"disabled collision fireball vanished on ground");bullets->Clear();
    BulletVisualConfig cannon;
    bullets->Fire({0,1002,0},{0,-10,0},engine::collisionCategories::Get("enemy_attack"),engine::collisionCategories::Get("player"),"Primitives/sphere",0.3f,3,cannon);
    bullets->Update(0.4f);Require(bullets->GetBullets().size()==1 && bullets->GetBullets().front()->GetTranslate().y<1000,"cannon ground pass changed");bullets->Clear();
    collisions->RemoveObject(floor.get());
    log<<"PASS preview/disabled collision and cannon ground-pass unchanged"<<std::endl;
}
inline void BombPhysicsCases(BaseScene* scene,std::ostream& log) {
    auto* collisions=engine::CollisionManager::GetInstance();
    auto floor=std::make_unique<engine::Object3D>();floor->Initialize(scene->GetObject3DCommon());floor->SetTranslate({0,1000,0});
    engine::Object3D::ColliderConfig box;box.type=engine::ColliderType::kAabb;box.size={100,0.1f,100};box.center={0,0,0};floor->SetColliderConfig(box);floor->SetCollisionAttribute(engine::collisionCategories::Get("ground"));floor->SetCollisionMask(~0u);floor->UpdateWorldMatrix(true);collisions->AddObject(floor.get());
    float shortest=10000,longest=0;
    for(bool playerOwned:{false,true})for(bool placed:{false,true})for(float deltaTime:{1.0f/30.0f,1.0f/60.0f,1.0f/144.0f,0.1f}) {
        auto actor=EnemyFactory::GetInstance()->CreateEnemy("Bomb",scene->GetObject3DCommon());auto* bomb=dynamic_cast<EnemyBomb*>(actor.get());Require(bomb,"missing bomb factory");
        bomb->SetTarget(nullptr);bomb->SetPlayerOwned(playerOwned);bomb->Ignite(8);bomb->SetTranslate({0,placed?1000.52f:1002.0f,0});
        bomb->BeginThrown(placed?engine::Vector3{0,1.8f,0}:engine::Vector3{0,8,24});bomb->UpdateWorldMatrix(true);
        const float radius=bomb->GetCollider()->GetRadius();bool landed=false;float landTime=0,landZ=0,landY=0;float elapsed=0;
        for(std::int32_t i=0;elapsed<3.0f;++i) {
            bomb->Update(deltaTime);bomb->OnCollision(floor.get());elapsed+=deltaTime;
            Require(std::abs(bomb->GetCollider()->GetRadius()-radius)<0.0001f,"bomb pulse changed collider radius");
            Require(!bomb->IsThrownPhysics(),"bomb entered generic slime bounce physics");
            Require(bomb->GetTranslate().y>=1000.1f+radius-0.035f,"bomb penetrated floor");
            if(!landed && bomb->IsGrounded()) {landed=true;landTime=elapsed;landZ=bomb->GetTranslate().z;landY=bomb->GetTranslate().y;}
            if(landed) {
                Require(bomb->GetTranslate().y<=landY+0.035f && bomb->GetVelocity().y<=0.05f,"bomb bounced after settling");
                if(elapsed-landTime>0.85f)Require(std::abs(bomb->GetVelocity().z)<0.01f,"bomb kept sliding after landing");
            }
        }
        Require(landed,"bomb never landed");Require(std::abs(bomb->GetTranslate().z-landZ)<3.2f,"bomb slid too far after landing");
        if(placed)Require(std::abs(bomb->GetTranslate().z)<0.01f,"placed bomb drifted horizontally");
        else {shortest=std::min(shortest,bomb->GetTranslate().z);longest=std::max(longest,bomb->GetTranslate().z);}
        log<<"PASS bomb physics owner="<<playerOwned<<" placed="<<placed<<" dt="<<deltaTime<<" land="<<landTime<<" slide="<<bomb->GetTranslate().z-landZ<<" finalZ="<<bomb->GetTranslate().z<<std::endl;
    }
    Require(longest-shortest<0.35f,"bomb throw range depended on frame rate");
    auto wall=std::make_unique<engine::Object3D>();wall->Initialize(scene->GetObject3DCommon());box.size={8,3,0.10f};wall->SetColliderConfig(box);wall->SetTranslate({0,1003,6});wall->SetCollisionAttribute(engine::collisionCategories::Get("ground"));wall->SetCollisionMask(~0u);wall->UpdateWorldMatrix(true);collisions->AddObject(wall.get());
    for(bool playerOwned:{false,true}) {
        auto actor=EnemyFactory::GetInstance()->CreateEnemy("Bomb",scene->GetObject3DCommon());auto* bomb=dynamic_cast<EnemyBomb*>(actor.get());bomb->SetTarget(nullptr);bomb->SetPlayerOwned(playerOwned);bomb->Ignite(8);bomb->SetTranslate({0,1002,0});bomb->BeginThrown({0,8,24});bomb->UpdateWorldMatrix(true);
        bomb->Update(0.1f);Require(std::abs(bomb->GetVelocity().y-2.0f)<0.01f && std::abs(bomb->GetTranslate().z-2.4f)<0.01f,"gravity doubled or air velocity changed");
        for(std::int32_t i=0;i<120;++i){bomb->Update(1.0f/60.0f);bomb->OnCollision(floor.get());bomb->OnCollision(wall.get());}
        Require(bomb->GetTranslate().z<5.7f && bomb->GetTranslate().z>0 && bomb->IsGrounded(),"wall contact tunneled or launched bomb");
    }
    collisions->RemoveObject(wall.get());collisions->RemoveObject(floor.get());
    log<<"PASS bomb wall contact, single gravity, stable placed/thrown physics and frame-rate range"<<std::endl;
}
// 爆弾モデルの縮小率・起爆経路が異なっても、同じワールド範囲へ一度だけ命中する。
inline void BombExplosionCases(BaseScene* scene, std::ostream& log) {
    auto* collisions = engine::CollisionManager::GetInstance();
    const engine::Vector3 origin{2000.0f, 1000.0f, 2000.0f};
    for (float scale : {0.16f, 0.30f, 2.0f}) for (bool direct : {false, true}) {
        std::vector<std::unique_ptr<engine::Object3D>> objects;
        /// <summary>検証中に登録した衝突対象を記録し、スコープを抜ける際にまとめて登録解除する。</summary>
        struct Registration {
            engine::CollisionManager* manager;
            std::vector<engine::Object3D*> values;
            ~Registration() { for (auto* object : values) manager->RemoveObject(object); }
        } registration{collisions, {}};
        auto addEnemy = [&](const engine::Vector3& offset) {
            auto actor = EnemyFactory::GetInstance()->CreateEnemy("Slime", scene->GetObject3DCommon());
            Require(actor && actor->GetParameters(), "bomb regression target missing");
            auto* enemy = actor.get();
            enemy->SetTarget(nullptr);
            enemy->SetParameter(&engine::EntityParameter::hp, enemy->SetParameter(&engine::EntityParameter::maxHp, 100.0f));
            enemy->SetScale({1, 1, 1});
            enemy->SetTranslate(origin + offset);
            engine::Object3D::ColliderConfig shape;
            shape.type = engine::ColliderType::kSphere; shape.size = {0.25f, 0.25f, 0.25f};
            enemy->SetColliderConfig(shape);
            enemy->SetCollisionAttribute(engine::collisionCategories::Get("enemy"));
            enemy->SetCollisionMask(engine::collisionCategories::Get("player_attack"));
            enemy->UpdateWorldMatrix(true);
            collisions->AddObject(enemy); registration.values.push_back(enemy);
            objects.push_back(std::move(actor));
            return enemy;
        };
        BaseEnemy* nearby = addEnemy({4.4f, 0, 0});
        BaseEnemy* outside = addEnemy({6.2f, 0, 0});
        BaseEnemy* contact = direct ? addEnemy({0, 0, 0.2f}) : nullptr;
        auto wall = std::make_unique<GimmickBreakableBlock>();
        wall->Initialize(scene->GetObject3DCommon(), "Stages/bomb_break_block");
        wall->SetScale({1, 1, 1}); wall->SetTranslate(origin + engine::Vector3{-4.4f, 0, 0});
        engine::Object3D::ColliderConfig box;
        box.type = engine::ColliderType::kAabb; box.size = {0.25f, 0.25f, 0.25f};
        wall->SetColliderConfig(box); wall->UpdateWorldMatrix(true);
        auto* breakable = wall.get();
        collisions->AddObject(breakable); registration.values.push_back(breakable);
        objects.push_back(std::move(wall));
        auto actor = EnemyFactory::GetInstance()->CreateEnemy("Bomb", scene->GetObject3DCommon());
        auto* bomb = dynamic_cast<EnemyBomb*>(actor.get());
        Require(bomb && bomb->GetParameters(), "bomb regression actor missing");
        bomb->SetTarget(nullptr); bomb->SetPlayerOwned(true);
        bomb->SetParameter(&engine::EntityParameter::attackPower, 1.0f);
        bomb->ApplyManagedScale({scale, scale, scale});
        bomb->SetTranslate(origin); bomb->SetRotation({0.4f, 0.6f, 0.2f});
        bomb->Ignite(direct ? 8.0f : 0.01f); bomb->UpdateWorldMatrix(true);
        if (direct) bomb->OnCollision(contact); else bomb->Update(0.02f);
        Require(bomb->IsExploded(), "player bomb failed to explode");
        Require(std::abs(bomb->GetCollider()->GetRadius() - 5.1f) < 0.01f, "bomb model scale shrank world blast");
        Require(std::abs(nearby->GetParameters()->hp - 90.0f) < 0.01f, "near-miss bomb did not damage nearby enemy once");
        Require(std::abs(outside->GetParameters()->hp - 100.0f) < 0.01f, "bomb damaged enemy outside blast");
        Require(breakable->IsDead() && breakable->GetCollisionAttribute() == 0, "fuse/impact blast did not break nearby wall");
        if (contact) Require(std::abs(contact->GetParameters()->hp - 90.0f) < 0.01f, "direct blast damage duplicated");
        // 次の衝突・更新に再通知しても同じ爆風を残さない。
        if (contact) bomb->OnCollision(contact);
        nearby->OnCollision(bomb); bomb->Update(0.02f);
        Require(std::abs(nearby->GetParameters()->hp - 90.0f) < 0.01f && bomb->GetCollisionAttribute() == 0,
                "expired bomb applied a second blast");
        // 爆発した後に近づいた敵へ、表示用の膨張エフェクトが追加ダメージを出さない。
        BaseEnemy* late = addEnemy({0, 0, 0});
        auto* meshes = MeshEffectManager::GetInstance();
        meshes->UpdateEditorPreviewStep(0.25f);
        for (const auto& visual : meshes->GetActiveEffects()) late->OnCollision(visual.get());
        Require(std::abs(late->GetParameters()->hp - 100.0f) < 0.01f, "visual blast applied delayed extra damage");
        log << "PASS player bomb blast scale=" << scale << " direct=" << direct
            << " radius=" << bomb->GetCollider()->GetRadius() << " nearHP=" << nearby->GetParameters()->hp
            << " outsideHP=" << outside->GetParameters()->hp << " wallBroken=" << breakable->IsDead() << std::endl;
    }
}
inline void AfterDraw(SceneManager* manager,const std::string& path) {
    static std::ofstream log(path,std::ios::trunc);static ULONGLONG sStart=GetTickCount64();
    static std::int32_t sPhase=0,sFrames=0;static bool sDone=false;static EnemyBomber* sBomber=nullptr;static EnemyBomb* sThrown=nullptr;
    static std::unique_ptr<engine::Camera> sCamera;static engine::Camera* sPrevious=nullptr;static engine::Vector3 sBase{},sTarget{},sRelease{};
    static engine::Matrix4x4 sInitialWvp{},sInitialViewProjection{};
    if(sDone || !manager)return;
    auto finish=[&](const std::string& message){log<<message<<std::endl;sDone=true;CameraManager::GetInstance()->SetActiveCamera(sPrevious);WinApp::CloseNow();};
    try {
        Require(GetTickCount64()-sStart<360000,"enemy projectile probe timeout");
        if(manager->IsTransitioning())return;
        if(sPhase==0) { StageManager::GetInstance()->SetCurrentStageById("stage1");manager->ChangeScene("GAMEPLAY");sPhase=1;return; }
        auto* scene=dynamic_cast<GamePlayScene*>(manager->GetCurrentScene());if(!scene)return;
        auto* player=GetScenePlayer(scene);if(!player)return;
        if(sPhase==1) {
            if(++sFrames<12 || !player->IsControlActive() || player->IsCinematicLocked())return;
            BombExplosionCases(scene,log);
            if (GetEnvironmentVariableA("CG2_BOMB_EXPLOSION_TEST", nullptr, 0) > 0) {
                finish("PASS: player bomb world radius, fuse/contact splash, breakable walls and single damage");
                return;
            }
            FireCases(scene,log);
            BombPhysicsCases(scene,log);
            Require(cannonDamageRuntimeProbe::RunCase(manager,0,0.6f,0,false,false,log),"cannon damage regression");
            Require(cannonDamageRuntimeProbe::RunPlayerProjectileCase(manager,log),"player projectile damage regression");
            manager->SetIsPlaying(true);
            sBase=player->GetWorldPosition()+engine::Vector3{0,2,8};sTarget=sBase+engine::Vector3{0,0,12};
            auto created=EnemyFactory::GetInstance()->CreateEnemy("Bomber",scene->GetObject3DCommon());
            sBomber=dynamic_cast<EnemyBomber*>(created.get());Require(sBomber,"bomber factory failed");
            sBomber->SetTranslate(sBase);sBomber->ApplyManagedScale({2,2,2});sBomber->SetTarget(player);
            sBomber->SetGravity(0);sBomber->SetMaxFallSpeed(0);sBomber->SetParameter(&engine::EntityParameter::gravity, sBomber->SetParameter(&engine::EntityParameter::maxFallSpeed, sBomber->SetParameter(&engine::EntityParameter::speed, 0)));
            sBomber->SetParameter(&engine::EntityParameter::hp, sBomber->SetParameter(&engine::EntityParameter::maxHp, 200));sBomber->SetVelocity({0,0,0});sBomber->UpdateWorldMatrix(true);
            sBomber->SetSpawnCallback([scene](std::unique_ptr<BaseEnemy> object){sThrown=dynamic_cast<EnemyBomb*>(object.get());scene->AddObject(std::move(object));});
            scene->AddObject(std::move(created));
            sPrevious=CameraManager::GetInstance()->GetActiveCameraOverride();sCamera=std::make_unique<engine::Camera>();sCamera->Initialize();sCamera->SetFovY(0.65f);
            engine::Camera::CameraOverrideParams view;view.duration=0;view.fixedEyePos=sBase+engine::Vector3{0,4,-13};view.fixedTargetPos=sBase+engine::Vector3{0,1,0};view.trackTargetX=view.trackTargetY=view.trackTargetZ=false;sCamera->StartOverride(view);sCamera->Update();CameraManager::GetInstance()->GetMainCamera()->StartOverride(view);CameraManager::GetInstance()->SetActiveCamera(sCamera.get());
            Access::Hold(sBomber);sPhase=2;sFrames=0;return;
        }
        if(sPhase==2) {
            auto* held=Access::Held(sBomber);Require(held && held->GetIsVisible(),"held bomb disappeared");
            const auto expected=Access::Expected(sBomber);
            Require(engine::Math::Length(held->GetWorldPosition()-expected)<0.002f,"held bomb did not follow final owner pose");
            auto* resource=held->GetMeshRenderer()->GetWvpResource();void* address=nullptr;D3D12_RANGE range{0,sizeof(engine::MeshRenderer::TransformationMatrix)};
            Require(SUCCEEDED(resource->Map(0,&range,&address)),"held bomb render buffer map failed");engine::MeshRenderer::TransformationMatrix gpu;std::memcpy(&gpu,address,sizeof(gpu));D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);
            Require(Same(gpu.world,held->GetWorldMatrix()),"held bomb world was not uploaded");
            const auto wvp=engine::Math::Multiply(held->GetWorldMatrix(),CameraManager::GetInstance()->GetActiveCamera()->GetViewProjectionMatrix());
            Require(Same(gpu.wvp,wvp,0.05f),"held bomb camera matrix was stale");
            if(++sFrames==8) {
                sInitialWvp=gpu.wvp;sInitialViewProjection=CameraManager::GetInstance()->GetActiveCamera()->GetViewProjectionMatrix();gateEntryRuntimeProbe::Capture(path+".held-camera-a.png");
                engine::Camera::CameraOverrideParams view;view.duration=0;view.fixedEyePos=sBase+engine::Vector3{6,5,-11};view.fixedTargetPos=sBase+engine::Vector3{0,1,0};view.trackTargetX=view.trackTargetY=view.trackTargetZ=false;sCamera->StartOverride(view);sCamera->Update();CameraManager::GetInstance()->GetMainCamera()->StartOverride(view);
                sBomber->SetTranslate(sBase+engine::Vector3{2,0,0});
            }
            if(sFrames==16) {
                Require(!Same(sInitialViewProjection,CameraManager::GetInstance()->GetActiveCamera()->GetViewProjectionMatrix(),0.01f),"test camera did not move");
                Require(!Same(sInitialWvp,gpu.wvp,0.05f),"held bomb stayed fixed on screen after camera moved");
                gateEntryRuntimeProbe::Capture(path+".held-camera-b.png");
                log<<"PASS held bomb GPU world/WVP follow final owner and moving camera"<<std::endl;
                Access::Release(sBomber);Require(!Access::Held(sBomber)->GetIsVisible(),"held model remained after release");Require(sThrown && !sThrown->IsExploded(),"real bomb did not spawn");sRelease=sThrown->GetTranslate();sPhase=3;sFrames=0;return;
            }
            Access::Hold(sBomber);return;
        }
        if(sPhase==3) {
            Require(!Access::Held(sBomber)->GetIsVisible(),"held model reappeared after throw");
            if(++sFrames<15)return;
            Require(sThrown && !sThrown->IsExploded() && engine::Math::Length(sThrown->GetTranslate()-sRelease)>0.1f,"thrown bomb did not fly in world");
            gateEntryRuntimeProbe::Capture(path+".thrown.png");
            log<<"PASS release hides held model and launches real world-space bomb"<<std::endl;
            finish("PASS: enemy fireball ground expiry, bomb physics, cannon regression and bomber moving-camera presentation");
        }
    }catch(const std::exception& error){gateEntryRuntimeProbe::Capture(path+".failed.png");finish(std::string("FAIL: ")+error.what());}
}
} // namespace enemyProjectileRuntimeProbe
#endif
