import XCTest
import Synchronization
@testable import ZerusKit

final class SessionNotificationPreferencesTests: XCTestCase {
    private let target = Target(computerId:"computer",session:"codex/project/session",run:"run",conversation:"conversation",connectionId:"workspace")
    private let signature = String(repeating:"a",count:64)
    private func baseline() -> JSONObject {
        ["reply_id":"reply-old","attention_signature":.string(signature),"optional_question_ids":[],"main_attention":.null,"child_attention":[:]]
    }
    private func preference(_ muted: Bool = false, revision: Int64 = 2, suppressed: JSONObject? = nil) -> JSONObject {
        ["conversation_id":.string(target.conversation),"muted":.bool(muted),"revision":.int(revision),"suppressed":.object(suppressed ?? baseline())]
    }
    private func raw(_ muted: Bool = false, suppressed: JSONObject? = nil) -> JSONObject {
        ["name":.string(target.session),"run_id":.string(target.run),"conversation_id":.string(target.conversation),"state":"running","tracked":true,
         "activity":"idle","phase":"idle","reply_id":"reply-old","attention_signature":.string(signature),"notification_preferences":.object(preference(muted,suppressed:suppressed))]
    }
    private func changed(_ raw: JSONObject, _ key: String, _ value: JSON) -> JSONObject { var copy = raw;copy[key] = value;return copy }
    private func attention(_ id: String = "request-old", phase: String = "input", started: JSON = 100) -> JSONObject {
        ["phase":.string(phase),"request_id":.string(id),"started_at":started]
    }
    private func session(_ raw: JSONObject) -> Session {
        Session(target:target,title:"Session",agent:"codex",status:"idle",project:"Project",preview:"",pendingCount:2,raw:raw)
    }
    private func current(_ raw: JSONObject, _ kind: SessionAlertKind?, optional: [String] = [], online: Bool = true) -> NotificationCurrent {
        NotificationCurrent(slot:NotificationSlot(connection:target.connectionId,computer:target.computerId,session:target.session),target:"run/conversation",
            title:"Session",machine:"Computer",kind:kind,identity:"identity",idle:true,online:online,conversation:target.conversation,optional:optional,
            suppressed:SessionNotificationPreferences.suppressed(raw,target,kind:kind,optional:optional))
    }
    private func plan(_ raw: JSONObject, _ kind: SessionAlertKind?, optional: [String] = [], previous: NotificationState = NotificationState(bootstrap:false,settingsGeneration:0),
                      eventKind: String = "attention", online: Bool = true) -> NotificationPlan {
        let now = current(raw,kind,optional:optional,online:online)
        var prefs = NotificationPreferences(generation:0);prefs.finished = true;prefs.optional = true
        return NotificationPolicy.plan(previous,current:[now],events:[(now.slot,NotificationEvent(id:10,computer:target.computerId,session:target.session,kind:eventKind,at:1000))],
            preferences:prefs,now:1000,active:[now.slot.key])
    }

    func testUnknownMalformedAndWrongConversationNeverInventASharedPreference() {
        XCTAssertNil(SessionNotificationPreferences.parse([:],target))
        XCTAssertNotNil(SessionNotificationPreferences.parse(raw(),target.with(run:"resumed")))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(),target.with(conversation:"fork")))
        for value in ["false",0,.null] as [JSON] {
            XCTAssertNil(SessionNotificationPreferences.parse(changed(raw(),"notification_preferences",.object(changed(preference(),"muted",value))),target))
        }
        for value in [-1,.double(2),"2",true] as [JSON] {
            XCTAssertNil(SessionNotificationPreferences.parse(changed(raw(),"notification_preferences",.object(changed(preference(),"revision",value))),target))
        }
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"attention_signature","not-a-hash")),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"reply_id","reply\nprivate")),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"child_attention",.object(JSONObject([("child\u{1}",.object(attention()))])))),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"optional_question_ids",["b","a"])),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"optional_question_ids",.array((0..<65).map { "id-\($0)" }.sorted().map(JSON.string)))),target))
        var children = JSONObject();for id in 0..<257 { children["child-\(id)"] = .object(attention()) }
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"child_attention",.object(children))),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"main_attention",.object(changed(attention(started:"100"),"run_id",.string(target.run))))),target))
    }

    func testExactFourFieldPayloadAndStrictCASArgumentsUseOuterRequestIdentityOnly() throws {
        let args = try SessionActionPolicies.arguments(SessionNotificationPreferences.operation,["muted":true,"expected_revision":2])
        let payload = SessionNotificationPreferences.payload(target,args)
        XCTAssertEqual(Set(["run_id","conversation_id","muted","expected_revision"]),Set(payload.keys))
        XCTAssertEqual(target.run,payload.string("run_id"));XCTAssertEqual(target.conversation,payload.string("conversation_id"))
        for value in ["true",1,.null] as [JSON] {
            XCTAssertThrowsError(try SessionActionPolicies.arguments(SessionNotificationPreferences.operation,["muted":value,"expected_revision":2]))
        }
        XCTAssertThrowsError(try SessionNotificationPreferences.arguments(changed(args,"request_id","extra")))
        XCTAssertThrowsError(try SessionNotificationPreferences.arguments(changed(args,"expected_revision",.double(2))))
    }

    func testReceiptsConfirmOnlyExactCASOutcomeAndNeverOptimisticallyChangeTheCatalog() {
        let row = raw();let original = row
        let action = SessionAction(requestId:"request",target:target,operation:SessionNotificationPreferences.operation,arguments:["muted":true,"expected_revision":2])
        func receipt(_ applied: Bool = true, _ revision: Int64 = 3) -> JSONObject {
            ["request_id":.string(action.requestId),"state":"completed","result":["ok":true,"applied":.bool(applied),"run_id":.string(target.run),
                "conversation_id":.string(target.conversation),"preferences":.object(preference(true,revision:revision))]]
        }
        XCTAssertEqual("completed",SessionActionPolicies.result(action,receipt()).status)
        XCTAssertEqual("completed",SessionActionPolicies.result(action,receipt(false,2)).status)
        let wrongRun = changed(receipt(),"result",.object(changed(receipt().object("result")!,"run_id","restarted")))
        let wrongOk = changed(receipt(),"result",.object(changed(receipt().object("result")!,"ok",false)))
        let wrongMute = changed(receipt(),"result",.object(changed(receipt().object("result")!,"preferences",.object(preference(false,revision:3)))))
        for wrong in [changed(receipt(),"request_id","other"),receipt(false,3),receipt(true,2),wrongRun,wrongOk,wrongMute] {
            XCTAssertEqual("uncertain",SessionActionPolicies.result(action,wrong).status)
        }
        XCTAssertEqual("failed",SessionActionPolicies.result(action,["request_id":"request","state":"failed","error":"conflict"]).status)
        for error in ["conflict","stale_identity","baseline_unavailable","not_found","capacity","revision_exhausted"] {
            let refused: JSONObject = ["request_id":"request","state":"completed","result":["ok":false,"error":.string(error)]]
            let result = SessionActionPolicies.result(action,refused)
            XCTAssertEqual("failed",result.status);XCTAssertFalse(result.blocksSending);XCTAssertFalse(result.error.isEmpty)
            XCTAssertEqual("uncertain",SessionActionPolicies.result(action,changed(refused,"request_id","other")).status)
        }
        XCTAssertEqual("uncertain",SessionActionPolicies.result(action,["request_id":"request","state":"completed","result":["ok":false,"error":"unknown"]]).status)
        XCTAssertEqual("uncertain",action.recover().status);XCTAssertEqual(original,row)
        XCTAssertEqual("",SessionActionPolicies.inspectReason(target,action.operation,action.arguments,row))
        XCTAssertTrue(SessionActionPolicies.inspectReason(target,action.operation,action.arguments,
            changed(row,"notification_preferences",.object(preference(revision:3)))).contains("changed"))
    }

    func testControlsRequireKnownABIAndLiveParentButKeepNativeFiltersUnreadAndPending() {
        var row = raw(true);row["phase"] = "input";row["mobile_unread_reply"] = true;row["pending_question_count"] = 2
        let s = session(row);let original = row
        var facts = SessionMenuFacts(target:target,row:row,state:"running",archived:false,online:true,supported:[SessionNotificationPreferences.operation])
        let item = SessionActionsMenu.items(facts).flatMap { $0 }.first { $0.id == SessionNotificationPreferences.operation }!
        XCTAssertTrue(item.enabled);XCTAssertEqual("Unmute notifications",item.label)
        facts.supported = [];XCTAssertEqual(SessionNotificationPreferences.unavailable,SessionActionsMenu.notificationReason(facts))
        facts.target = target.with(archiveId:"archive");XCTAssertFalse(SessionActionsMenu.notificationReason(facts).isEmpty)
        facts.target = target.with(agentId:"child");XCTAssertFalse(SessionActionsMenu.notificationReason(facts).isEmpty)
        XCTAssertTrue(SessionFilters.needsAttention(s,true));XCTAssertTrue(SessionFilters.active(s,true))
        XCTAssertEqual(.neutral,DesktopSessionPalette.badge(s,true).kind)
        XCTAssertTrue(SessionFilters.visibleUnreadReply(s,true));XCTAssertEqual(2,s.pendingCount)
        XCTAssertEqual(.working,DesktopSessionPalette.badge(session(changed(raw(true),"activity","busy")),true).kind)
        XCTAssertEqual(.error,DesktopSessionPalette.badge(session(changed(raw(true),"phase","error")),true).kind)
        XCTAssertEqual(original,row)
    }

    func testMutedAndOfflineThroughUnmuteBaselineWithdrawCardsWithoutReplayOrOverflow() {
        var muted = raw(true);muted["phase"] = "input";muted["attention_id"] = "request-old"
        let slot = current(muted,.input).slot
        let old = NotificationState(bootstrap:false,settingsGeneration:0,records:[NotificationRecord(slot:slot,target:"run/conversation",observedAt:900,postedFingerprint:"old",postedKind:.input)])
        let quiet = plan(muted,.input,previous:old,online:false)
        XCTAssertTrue(quiet.desired.isEmpty);XCTAssertTrue(quiet.retained.isEmpty);XCTAssertFalse(quiet.overflow)
        let baseline = changed(baseline(),"main_attention",.object(changed(attention(),"run_id",.string(target.run))))
        var resumed = raw(suppressed:baseline);resumed["phase"] = "input";resumed["attention_id"] = "request-old";resumed["attention_started"] = 9000
        XCTAssertTrue(plan(resumed,.input).desired.isEmpty)
        XCTAssertTrue(plan(raw(suppressed:baseline),nil,eventKind:"completed").desired.isEmpty)
        XCTAssertFalse(SessionNotificationPreferences.suppressed(changed(raw(suppressed:baseline),"reply_id","reply-new"),target,kind:nil,optional:[]))
        XCTAssertEqual(true,plan(changed(resumed,"attention_id","request-new"),.input).desired.first?.fresh)
    }

    func testIndependentMainSiblingOptionalAndTimestampBaselinesNeverHideNewIdentities() {
        var baseline = baseline();baseline["main_attention"] = .object(changed(attention(),"run_id",.string(target.run)))
        baseline["optional_question_ids"] = ["optional-old"];baseline["child_attention"] = ["child-a":.object(attention("child-request",phase:"approval"))]
        var row = raw(suppressed:baseline);row["phase"] = "input";row["attention_id"] = "request-old"
        row["subagents"] = ["child-a":["state":"approval","attention_id":"child-request","attention_started":99999]]
        XCTAssertTrue(plan(row,.input).desired.isEmpty)
        let siblings = changed(row.object("subagents")!,"child-b",["state":"input","question_id":"new-sibling"])
        XCTAssertEqual(true,plan(changed(row,"subagents",.object(siblings)),.input).desired.first?.fresh)
        XCTAssertTrue(plan(raw(suppressed:baseline),.optional,optional:["optional-old"]).desired.isEmpty)
        XCTAssertEqual(true,plan(raw(suppressed:baseline),.optional,optional:["optional-old","optional-new"]).desired.first?.fresh)
        let noId = changed(self.baseline(),"main_attention",.object(changed(attention("",started:100),"run_id",.string(target.run))))
        var fallback = raw(suppressed:noId);fallback["phase"] = "input";fallback["attention_started"] = 100
        XCTAssertTrue(plan(fallback,.input).desired.isEmpty)
        XCTAssertEqual(true,plan(changed(fallback,"attention_started",101),.input).desired.first?.fresh)
        let noClock = changed(self.baseline(),"main_attention",.object(changed(attention("",started:.null),"run_id",.string(target.run))))
        let unknown = changed(raw(suppressed:noClock),"phase","input")
        XCTAssertNil(SessionNotificationPreferences.parse(unknown,target))
        XCTAssertFalse(SessionNotificationPreferences.suppressed(unknown,target,kind:.input,optional:[]))
        let childClock = changed(self.baseline(),"child_attention",["child-a":.object(attention("",phase:"approval",started:100))])
        var child = raw(suppressed:childClock);child["subagents"] = ["child-a":["state":"approval","last_event_at":100]]
        XCTAssertTrue(plan(child,.input).desired.isEmpty)
        child["subagents"] = ["child-a":["state":"approval","last_event_at":101]]
        XCTAssertEqual(true,plan(child,.input).desired.first?.fresh)
        let decoyRead = changed(raw(),"read",["reply_id":"reply-new"])
        XCTAssertTrue(SessionNotificationPreferences.suppressed(decoyRead,target,kind:nil,optional:[]))
    }

    func testPreferenceEventsOnlyRefreshAndCannotBecomeAudibleTransitions() {
        var row = raw();row["phase"] = "input";row["attention_id"] = "new-request"
        XCTAssertEqual(false,plan(row,.input,eventKind:"notification_preferences").desired.first?.fresh)
    }

    private func confirmed(_ muted: Bool, revision: Int64, bound: Target? = nil, suppressed: JSONObject? = nil) -> SessionAction {
        let target = bound ?? self.target
        let action = SessionAction(requestId:"receipt-\(revision)-\(target.computerId)-\(target.conversation)",target:target,
            operation:SessionNotificationPreferences.operation,arguments:["muted":.bool(muted),"expected_revision":.int(revision - 1)])
        var preference = preference(muted,revision:revision,suppressed:suppressed);preference["conversation_id"] = .string(target.conversation)
        return SessionActionPolicies.result(action,["request_id":.string(action.requestId),"state":"completed","result":["ok":true,"applied":true,
            "run_id":.string(target.run),"conversation_id":.string(target.conversation),"preferences":.object(preference)]])
    }

    func testPreviouslyParsedCatalogAndDetailCannotUndoConfirmedMuteButHigherRevisionCan() {
        let bound = target.with(computerId:"22222222-2222-4222-8222-222222222222")
        let connection = Connection(id:target.connectionId,label:"Fixture",url:"https://relay.example.test",token:"synthetic")
        var old = raw();old["phase"] = "input";old["attention_id"] = "old-input"
        let machine: JSONObject = ["id":.string(bound.computerId),"name":"Computer","online":true,"snapshot":["sessions":[.object(old)]]]
        let parsedBeforeReceipt = SessionCatalogParser.parse(connection,[machine]).projects.sessions
        let oldNotice = NotificationCatalogParser.parse(connection,records:[machine],aliases:[]).sessions[0]
        let actions = [confirmed(true,revision:3,bound:bound)]
        let row = SessionNotificationPreferences.mergeRows(parsedBeforeReceipt,retained:[],actions:actions)[0]
        XCTAssertTrue(SessionNotificationPreferences.muted(row))
        XCTAssertEqual(3,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(old,bound,retained:[row.raw],actions:actions),bound)?.revision)
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(oldNotice,actions:actions))
        let renamed = bound.with(session:"codex/project/renamed",run:"resumed")
        let conflicting = changed(raw(),"notification_preferences",.object(preference(false,revision:3)))
        XCTAssertEqual(true,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(conflicting,renamed,retained:[row.raw],actions:actions),renamed)?.muted)
        let higher = changed(raw(),"notification_preferences",.object(preference(false,revision:4)))
        XCTAssertEqual(false,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(higher,renamed,retained:[row.raw],actions:actions),renamed)?.muted)
        XCTAssertNil(SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail([:],bound.with(conversation:"fork"),retained:[],actions:actions),conversation:"fork"))
        var freshNotice = oldNotice;freshNotice.notificationPreference = SessionNotificationPreferences.parse(higher,bound)
        XCTAssertTrue(SessionNotificationPreferences.deliveryAllowed(freshNotice,actions:actions))
        XCTAssertTrue(NotificationCatalogParser.parse(connection,records:[machine],aliases:[],actions:actions).sessions[0].suppressed)
    }

    func testConfirmedUnmuteBaselineBlocksOfflineReplayAndQueuedOldNotices() {
        var old = raw();old["phase"] = "input";old["attention_id"] = "request-old"
        let suppressed = changed(baseline(),"main_attention",.object(changed(attention(),"run_id",.string(target.run))))
        let actions = [confirmed(true,revision:3),confirmed(false,revision:4,suppressed:suppressed)]
        var merged = SessionNotificationPreferences.mergeDetail(old,target,retained:[],actions:actions)
        XCTAssertEqual(false,SessionNotificationPreferences.parse(merged,target)?.muted)
        XCTAssertTrue(plan(merged,.input).desired.isEmpty)
        XCTAssertTrue(plan(merged,nil,eventKind:"completed").desired.isEmpty)
        var queued = current(old,.input);queued.notificationPreference = SessionNotificationPreferences.parse(old,target)
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(queued,actions:actions))
        merged["attention_id"] = "new-input"
        XCTAssertEqual(true,plan(merged,.input).desired.first?.fresh)
    }

    func testProofHistoryKeepsHighestRevisionAndOneMiBWithoutLosingReceipts() throws {
        var state = MessageState().action(confirmed(true,revision:5)).action(confirmed(false,revision:3))
        XCTAssertNotNil(state.sessionActions[0].confirmedNotificationPreference)
        XCTAssertNil(state.sessionActions[1].confirmedNotificationPreference)
        var invalid = confirmed(false,revision:99);invalid.requestId = "failed-proof";invalid.status = "failed"
        var malformed = confirmed(true,revision:5);malformed.requestId = "malformed-proof";malformed.confirmedNotificationPreference = "{broken"
        let filtered = state.action(invalid).action(malformed)
        XCTAssertTrue(filtered.sessionActions.suffix(2).allSatisfy { $0.confirmedNotificationPreference == nil })
        XCTAssertEqual(5,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(raw(),target,retained:[],actions:filtered.sessionActions),target)?.revision)
        var large = baseline();large["main_attention"] = .object(changed(attention(String(repeating:"x",count:60000)),"run_id",.string(target.run)))
        large["child_attention"] = ["one":.object(attention(String(repeating:"x",count:60000))),"two":.object(attention(String(repeating:"x",count:60000))),
                                   "three":.object(attention(String(repeating:"x",count:60000)))]
        for index in 0..<8 { state = state.action(confirmed(true,revision:6,bound:target.with(conversation:"conversation-\(index)"),suppressed:large)) }
        XCTAssertEqual(10,state.sessionActions.count)
        let proofs = state.sessionActions.compactMap(\.confirmedNotificationPreference)
        XCTAssertTrue((1...5).contains(proofs.count))
        XCTAssertLessThanOrEqual(proofs.reduce(0) { $0 + JSON.string($1).compact.utf8.count },1024 * 1024)
        XCTAssertTrue(state.sessionActions.allSatisfy { $0.status == "completed" && !$0.blocksSending })
        let recovered = try JSONDecoder().decode(MessageState.self,from:JSONEncoder().encode(state)).recovered()
        XCTAssertEqual(state.sessionActions.map(\.requestId),recovered.sessionActions.map(\.requestId))
        XCTAssertEqual(proofs,recovered.sessionActions.compactMap(\.confirmedNotificationPreference))
    }

    func testBaselineBoundsUseUTF8AndAllowOnlyLFForDeepSeekCompositeRequestIDs() {
        let composite = "question-one\nquestion-two"
        let suppressed = changed(baseline(),"main_attention",.object(changed(attention(composite),"run_id",.string(target.run))))
        var row = raw(suppressed:suppressed);row["phase"] = "input";row["attention_id"] = .string(composite)
        XCTAssertNotNil(SessionNotificationPreferences.parse(row,target))
        XCTAssertTrue(SessionNotificationPreferences.suppressed(row,target,kind:.input,optional:[]))
        row["attention_id"] = "question-two\nquestion-one"
        XCTAssertFalse(SessionNotificationPreferences.suppressed(row,target,kind:.input,optional:[]))
        for id in ["x\rnext","x\tnext","x\u{7f}next",String(repeating:"é",count:32769)] {
            XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"main_attention",.object(changed(attention(id),"run_id",.string(target.run))))),target))
        }
        XCTAssertNotNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"main_attention",.object(changed(attention(String(repeating:"é",count:32768)),"run_id",.string(target.run))))),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"reply_id",.string(String(repeating:"é",count:129)))),target))
        let longPhase = changed(attention(),"phase",.string(String(repeating:"é",count:129)))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"main_attention",.object(changed(longPhase,"run_id",.string(target.run))))),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"child_attention",["child":.object(attention("",started:.null))])),target))
        let opaque = changed(baseline(),"reply_id",.string("é"))
        XCTAssertFalse(SessionNotificationPreferences.suppressed(changed(raw(suppressed:opaque),"reply_id",.string("e\u{301}")),target,kind:nil,optional:[]))
    }

    func testEqualOpaqueConversationIDsFromDifferentProvidersNeverShareProofOrMute() {
        let proof = confirmed(true,revision:3),foreign = target.with(session:"claude/project/session")
        XCTAssertNotEqual(SessionNotificationPreferences.key(target),SessionNotificationPreferences.key(foreign))
        XCTAssertFalse(SessionNotificationPreferences.sameConversation(target,foreign))
        let original = session(changed(raw(true),"notification_preferences",.object(preference(true,revision:3))))
        var other = session(raw());other.target = foreign
        let merged = SessionNotificationPreferences.mergeRows([other],retained:[original],actions:[proof])[0]
        XCTAssertFalse(SessionNotificationPreferences.muted(merged))
        XCTAssertEqual(false,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(other.raw,foreign,retained:[],actions:[proof]),foreign)?.muted)
        var notice = current(raw(),.input);notice.slot = NotificationSlot(connection:foreign.connectionId,computer:foreign.computerId,session:foreign.session)
        XCTAssertTrue(SessionNotificationPreferences.deliveryAllowed(notice,actions:[proof]))
        XCTAssertTrue(SessionNotificationPreferences.sameConversation(target,target.with(session:"codex/other/renamed",run:"resumed")))
    }

    func testCompletePreferenceRejectsOversizeASCIIEscapingAndUnknownShapes() {
        let request = String(repeating:"é",count:32768)
        let one = changed(baseline(),"main_attention",.object(changed(attention(request),"run_id",.string(target.run))))
        XCTAssertNotNil(SessionNotificationPreferences.parse(raw(suppressed:one),target))
        let oversized = changed(one,"child_attention",["child":.object(attention(request))])
        let value = raw(suppressed:oversized)
        XCTAssertLessThan(JSON.object(value.object("notification_preferences")!).compact.utf8.count,384 * 1024)
        XCTAssertNil(SessionNotificationPreferences.parse(value,target))
        XCTAssertNil(SessionNotificationPreferences.parse(changed(raw(),"notification_preferences",.object(changed(preference(),"future",true))),target))
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"future",true)),target))
        let edge = changed(changed(attention(),"run_id",.string(target.run)),"future",true)
        XCTAssertNil(SessionNotificationPreferences.parse(raw(suppressed:changed(baseline(),"main_attention",.object(edge))),target))
        var action = confirmed(true,revision:3);action.status = "sending";action.confirmedNotificationPreference = nil
        let receipt: JSONObject = ["request_id":.string(action.requestId),"state":"completed","result":["ok":true,"applied":true,
            "run_id":.string(target.run),"conversation_id":.string(target.conversation),"preferences":.object(preference(true,revision:3,suppressed:oversized))]]
        XCTAssertEqual("uncertain",SessionActionPolicies.result(action,receipt).status)
        XCTAssertNil(SessionActionPolicies.result(action,receipt).confirmedNotificationPreference)
    }

    private func parsedCurrent(_ value: JSONObject) -> NotificationCurrent {
        let connection = Connection(id:target.connectionId,label:"Fixture",url:"https://relay.example.test",token:"synthetic")
        let machine: JSONObject = ["id":"22222222-2222-4222-8222-222222222222","name":"Computer","online":true,"snapshot":["sessions":[.object(value)]]]
        return NotificationCatalogParser.parse(connection,records:[machine],aliases:[]).sessions[0]
    }
    private func catalogPlan(_ raw: JSONObject, previous: NotificationState = NotificationState(bootstrap:false,settingsGeneration:0), at: Double = 1000) -> NotificationPlan {
        let current = parsedCurrent(raw)
        var preferences = NotificationPreferences(generation:0);preferences.finished = true;preferences.optional = true
        return NotificationPolicy.plan(previous,current:[current],events:[(current.slot,NotificationEvent(id:Int64(at),computer:current.slot.computer,session:current.slot.session,kind:"attention",at:at))],
            preferences:preferences,now:at)
    }

    func testRealCatalogFingerprintAnnouncesNewFallbackClocksButIgnoresClockForOwnedID() {
        let baseline = changed(baseline(),"main_attention",.object(changed(attention("",started:100),"run_id",.string(target.run))))
        var old = raw(suppressed:baseline);old["phase"] = "input";old["attention_started"] = 100
        let first = catalogPlan(old);XCTAssertTrue(first.desired.isEmpty)
        let next = changed(old,"attention_started",101)
        let second = catalogPlan(next,previous:first.state,at:1001);XCTAssertEqual(true,second.desired.first?.fresh)
        let third = catalogPlan(changed(next,"attention_started",102),previous:second.state,at:1002)
        XCTAssertEqual(true,third.desired.first?.fresh)
        let identified = changed(next,"attention_id","new-owned-id")
        let owned = catalogPlan(identified,previous:third.state,at:1003);XCTAssertEqual(true,owned.desired.first?.fresh)
        let clockOnly = catalogPlan(changed(identified,"attention_started",999),previous:owned.state,at:1004)
        XCTAssertEqual(false,clockOnly.desired.first?.fresh)
    }

    func testRealCatalogKeepsCoveredSiblingQuietAfterRemovalsAndAnnouncesNewChildOrOptional() {
        func child(_ id: String) -> JSONObject { ["state":"approval","attention_id":.string(id),"attention_started":100] }
        func optional(_ id: String) -> JSONObject { ["source":"codex_async","optional":true,"question_id":.string(id),"question_hash":.string("hash-" + id)] }
        var suppressed = baseline();suppressed["child_attention"] = ["a":.object(attention("a",phase:"approval")),"b":.object(attention("b",phase:"approval"))]
        suppressed["optional_question_ids"] = ["optional-old"]
        var original = raw(suppressed:suppressed);original["subagents"] = ["a":.object(child("a")),"b":.object(child("b"))]
        original["mobile_attention"] = [.object(optional("optional-old"))]
        let first = catalogPlan(original);XCTAssertTrue(first.desired.isEmpty)
        var remaining = original;remaining["subagents"] = ["b":.object(child("b"))];remaining["attention_signature"] = .string(String(repeating:"b",count:64))
        let second = catalogPlan(remaining,previous:first.state,at:1001);XCTAssertTrue(second.desired.isEmpty)
        var removed = remaining;removed["mobile_attention"] = [];removed["attention_signature"] = .string(String(repeating:"c",count:64))
        let third = catalogPlan(removed,previous:second.state,at:1002);XCTAssertTrue(third.desired.isEmpty)
        var newChild = removed;newChild["subagents"] = ["b":.object(child("b")),"new":.object(child("new"))]
        let fourth = catalogPlan(newChild,previous:third.state,at:1003);XCTAssertEqual(true,fourth.desired.first?.fresh)
        var changedChild = newChild;changedChild["subagents"] = ["b":.object(child("b")),"new":.object(child("new-again"))]
        let fifth = catalogPlan(changedChild,previous:fourth.state,at:1004);XCTAssertEqual(true,fifth.desired.first?.fresh)
        var newOptional = removed;newOptional["mobile_attention"] = [.object(optional("optional-new"))]
        let sixth = catalogPlan(newOptional,previous:third.state,at:1005)
        XCTAssertEqual(.optional,sixth.desired.first?.kind);XCTAssertEqual(true,sixth.desired.first?.fresh)
        let unknownMain = changed(removed,"pending_question_count",1)
        XCTAssertFalse(parsedCurrent(unknownMain).suppressed)
        XCTAssertEqual(true,catalogPlan(unknownMain,previous:third.state,at:1006).desired.first?.fresh)
    }

    func testPendingOrUncertainMuteBlocksQueuedDeliveryAndKnownNoWriteFailureReleasesIt() {
        let now = parsedCurrent(raw()),bound = target.with(computerId:"22222222-2222-4222-8222-222222222222")
        let pending = SessionAction(requestId:"pending",target:bound,operation:SessionNotificationPreferences.operation,arguments:["muted":true,"expected_revision":2])
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(now,actions:[pending]))
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(now,actions:[pending.recover()]))
        let refused = SessionActionPolicies.result(pending,["request_id":"pending","state":"completed","result":["ok":false,"error":"conflict"]])
        XCTAssertEqual("failed",refused.status);XCTAssertFalse(refused.blocksSending)
        XCTAssertTrue(SessionNotificationPreferences.deliveryAllowed(now,actions:[refused]))
        XCTAssertTrue(SessionNotificationPreferences.known(actions:[pending]).isEmpty)
    }

    private final class FailingSink: Sendable {
        let failing = Mutex(true)
        func write(_ state: MessageState) throws { if failing.withLock({ $0 }) { throw ZerusError.invalid("disk full") } }
    }
    @MainActor func testFailedReceiptPersistenceKeepsOriginalRecoveryAndQueuedNoticeBlocked() async throws {
        let now = parsedCurrent(raw()),bound = target.with(computerId:"22222222-2222-4222-8222-222222222222")
        let success = confirmed(true,revision:3,bound:bound)
        var pending = success;pending.status = "sending";pending.confirmedNotificationPreference = nil
        let sink = FailingSink()
        let writer = OrderedStatePersistence(initial:MessageState(sessionActions:[pending])) { try sink.write($0) }
        defer { writer.close() }
        do { _ = try await writer.durable { $0.action(success) };XCTFail("Receipt write must fail") } catch {}
        XCTAssertEqual("sending",writer.state.sessionActions[0].status)
        XCTAssertNil(writer.state.sessionActions[0].confirmedNotificationPreference)
        XCTAssertTrue(SessionNotificationPreferences.known(actions:writer.state.sessionActions).isEmpty)
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(now,actions:writer.state.sessionActions))
        sink.failing.withLock { $0 = false }
        _ = try await writer.durable { $0.action(success) }
        XCTAssertEqual("completed",writer.state.sessionActions[0].status)
        XCTAssertNotNil(writer.state.sessionActions[0].confirmedNotificationPreference)
        XCTAssertFalse(SessionNotificationPreferences.deliveryAllowed(now,actions:writer.state.sessionActions))
    }

    func testCoveredMainCannotHideNewRecoveryOrUnaccountedBlockingQuestions() {
        let suppressed = changed(baseline(),"main_attention",.object(changed(attention("main-a"),"run_id",.string(target.run))))
        var original = raw(suppressed:suppressed);original["phase"] = "input";original["attention_id"] = "main-a";original["attention_started"] = 100
        let first = catalogPlan(original);XCTAssertTrue(first.desired.isEmpty)
        for state in ["exhausted","blocked","uncertain"] {
            let recovery = changed(original,"recovery",["state":.string(state),"request_id":"recovery-b","error":"new failure"])
            XCTAssertFalse(parsedCurrent(recovery).suppressed)
            let next = catalogPlan(recovery,previous:first.state,at:1001)
            XCTAssertEqual(.error,next.desired.first?.kind);XCTAssertEqual(true,next.desired.first?.fresh)
        }
        var unknown = original;unknown["pending_question_count"] = 1;unknown["attention_signature"] = .string(String(repeating:"b",count:64))
        XCTAssertFalse(parsedCurrent(unknown).suppressed)
        XCTAssertEqual(true,catalogPlan(unknown,previous:first.state,at:1002).desired.first?.fresh)
        var blocking = original;blocking["mobile_attention"] = [["question_id":"new-blocking","question_hash":"new-hash","optional":false]]
        blocking["attention_signature"] = .string(String(repeating:"c",count:64))
        XCTAssertFalse(parsedCurrent(blocking).suppressed)
        XCTAssertEqual(true,catalogPlan(blocking,previous:first.state,at:1003).desired.first?.fresh)
    }
}
