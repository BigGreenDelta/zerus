import XCTest
@testable import ZerusKit

final class RepositoryAcquisitionTests: XCTestCase {
    private let id="11111111-1111-4111-8111-111111111111"
    private let nativeId="22222222-2222-4222-8222-222222222222"
    private var machine: MachineKey { MachineKey(connectionId:"workspace",computerId:"computer") }
    private func action() throws -> SessionAction {
        SessionAction(requestId:id,target:Target(computerId:"computer",session:"",run:"",conversation:"",connectionId:"workspace"),operation:RepositoryAcquisition.clone,
            arguments:try RepositoryAcquisition.arguments(machineId:nativeId,url:"git@forge:owner/source.git",path:"/fixture/source"))
    }
    private func receipt(_ readId:String?=nil)->JSONObject {
        let readId=readId ?? id
        return ["request_id":.string(readId),"state":"completed","result":["schema":1,"request_id":.string(readId),"clone_request_id":.string(id),
            "machine_id":.string(nativeId),"status":"cloned","parent":"/fixture","name":"source","path":"/fixture/source"]]
    }
    func testSuggestedNameAndManualDestinationAreIndependent() throws {
        XCTAssertEqual(RepositoryAcquisition.suggestedName("https://example.test/owner/source.git"),"source")
        XCTAssertEqual(RepositoryAcquisition.suggestedName("git@forge:owner/source.git"),"source")
        XCTAssertEqual(RepositoryAcquisition.suggestedName("forge:repo"),"repo")
        XCTAssertEqual(RepositoryAcquisition.destination("/","source"),"/source")
        XCTAssertEqual(try RepositoryAcquisition.arguments(machineId:nativeId,url:"git@forge:other.git",path:"/fixture/custom").string("name"),"custom")
        for url in ["C:/source","C:\\source","/source","file:///source","ext::command","https://token@example.test/repo","ssh://user:password@example.test/repo","https://example.test/repo?token=value"] {
            XCTAssertThrowsError(try RepositoryAcquisition.arguments(machineId:nativeId,url:url,path:"/fixture/source"))
        }
    }
    func testReceiptBindsComputerOriginalUuidAndDestination() throws {
        let a=try action()
        XCTAssertEqual(SessionActionPolicies.result(a,receipt()).resultPath,"/fixture/source")
        for key in ["request_id","clone_request_id","machine_id","parent","path","name"] {
            var r=receipt(),raw=r.object("result")!;raw[key]="other";r["result"] = .object(raw)
            XCTAssertEqual(SessionActionPolicies.result(a,r).status,"uncertain",key)
        }
    }
    func testRestartRecoveryHasNoSessionAuthority() throws {
        let interrupted=try action().recover()
        XCTAssertEqual(interrupted.status,"uncertain")
        XCTAssertNotNil(RepositoryAcquisition.pending([interrupted],machine))
        XCTAssertNotNil(RepositoryAcquisition.pending([interrupted],machine,path:"/fixture/source"))
        XCTAssertNil(RepositoryAcquisition.pending([interrupted],machine,path:"/fixture/different"))
        var corrupt=interrupted;corrupt.arguments=[:]
        XCTAssertNotNil(RepositoryAcquisition.pending([corrupt],machine,path:"/fixture/different"))
        let confirmed=RepositoryAcquisition.result(interrupted,receipt(nativeId),readId:nativeId)
        XCTAssertEqual(confirmed.status,"completed");XCTAssertNil(confirmed.resultTarget)
        XCTAssertEqual(RepositoryAcquisition.result(interrupted,["request_id":.string(nativeId),"state":"failed"],readId:nativeId).status,"uncertain")
    }
    func testUnknownAndFailedAreNeverReady() throws {
        let a=try action();var r=receipt(),raw=r.object("result")!
        raw["status"]="unconfirmed";r["result"] = .object(raw)
        XCTAssertTrue(SessionActionPolicies.result(a,r).blocksSending)
        raw["status"]="failed";raw["detail"]="Git refused";r["result"] = .object(raw)
        XCTAssertEqual(SessionActionPolicies.result(a,r).status,"failed")
    }
    func testCanonicalReadOnlyCheckProposesDestinationButMutationStaysExact() throws {
        let requested=try RepositoryAcquisition.arguments(machineId:nativeId,url:"git@forge:owner/source.git",path:"/var/tmp/source")
        var ready=receipt(nativeId),raw=ready.object("result")!
        raw["clone_request_id"] = .string(nativeId);raw["status"]="ready";raw["parent"]="/private/var/tmp";raw["path"]="/private/var/tmp/source";ready["result"] = .object(raw)
        let decision=try RepositoryAcquisition.checkedDestination(requested,ready,id:nativeId)
        XCTAssertTrue(decision.needsConfirmation);XCTAssertEqual(decision.path,"/private/var/tmp/source")
        let canonical=decision.arguments
        XCTAssertFalse(try RepositoryAcquisition.checkedDestination(canonical,ready,id:nativeId).needsConfirmation)
        XCTAssertEqual(canonical.string("parent"),"/private/var/tmp");XCTAssertEqual(canonical.string("url"),requested.string("url"))
        XCTAssertNotEqual(canonical.string("parent"),requested.string("parent"))
        var original=try action();original.arguments=requested
        var changed=receipt(),changedRaw=changed.object("result")!
        changedRaw["parent"]="/private/var/tmp";changedRaw["path"]="/private/var/tmp/source";changed["result"] = .object(changedRaw)
        XCTAssertEqual(RepositoryAcquisition.result(original,changed).status,"uncertain")
        changed["request_id"] = .string(nativeId);changedRaw["request_id"] = .string(nativeId);changed["result"] = .object(changedRaw)
        XCTAssertEqual(RepositoryAcquisition.result(original,changed,readId:nativeId).status,"uncertain")
        raw["name"]="other";ready["result"] = .object(raw)
        XCTAssertThrowsError(try RepositoryAcquisition.checkedDestination(requested,ready,id:nativeId))
    }
    func testEveryUnresolvedDestinationHasOriginalStatusRecoveryAndBoundedStablePages() throws {
        var actions:[SessionAction]=[]
        for index in 1...13 {
            var a=try action();a.requestId=String(format:"00000000-0000-4000-8000-%012d",index);a.status="uncertain"
            a.arguments=try RepositoryAcquisition.arguments(machineId:nativeId,url:"git@forge:owner/source.git",path:"/fixture/source\(index)");actions.append(a)
        }
        for index in [1,2] {
            let page=RepositoryAcquisition.recovery(actions,machine,path:"/fixture/source\(index)")
            XCTAssertEqual(page.selected?.requestId,actions[index-1].requestId)
            let read=try RepositoryAcquisition.statusPayload(page.selected!,id:nativeId)
            XCTAssertEqual(read.string("clone_request_id"),actions[index-1].requestId);XCTAssertEqual(read.string("name"),"source\(index)")
            XCTAssertEqual(read.string("request_id"),nativeId);XCTAssertEqual(page.others.count,5)
        }
        var found=Set<String>(),cursor=""
        repeat {
            let page=RepositoryAcquisition.recovery(Array(actions.reversed()),machine,path:"/fixture/new",before:cursor)
            XCTAssertLessThanOrEqual(page.others.count,5)
            for action in page.others { XCTAssertTrue(found.insert(action.requestId).inserted) };cursor=page.next ?? ""
        } while !cursor.isEmpty
        XCTAssertEqual(found,Set(actions.map(\.requestId)))
        XCTAssertNil(RepositoryAcquisition.pending(actions,machine,path:"/fixture/new"))
        XCTAssertTrue(RepositoryAcquisition.recovery(actions,MachineKey(connectionId:"other",computerId:machine.computerId),path:"/fixture/source1").others.isEmpty)
    }
    func testFreshSelectedCheckoutProofBeforeLaunch() {
        var raw:JSONObject=["state":"ok","stale":false,"partial":true,"path":"/fixture/source","requested_path":"/fixture/source",
            "common_dir":"/fixture/source/.git","worktrees":[["path":"/fixture/source","kind":"main","available":true]]]
        XCTAssertTrue(RepositoryAcquisition.verifiedCheckout(raw,path:"/fixture/source"))
        XCTAssertFalse(RepositoryAcquisition.verifiedCheckout(raw,path:"/fixture/other"))
        raw["stale"]=true;XCTAssertFalse(RepositoryAcquisition.verifiedCheckout(raw,path:"/fixture/source"))
        raw["stale"]=false;raw["worktrees"]=[["path":"/fixture/source","kind":"main","available":false]]
        XCTAssertFalse(RepositoryAcquisition.verifiedCheckout(raw,path:"/fixture/source"))
    }
}
