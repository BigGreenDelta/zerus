import XCTest
@testable import ZerusKit

final class JSONTests: XCTestCase {
    func testParsesNestedValuesAndKeepsObjectKeyOrder() throws {
        let value = try JSON.parse(Data(#"{"b":1,"a":[true,null,"x",2.5],"c":{"z":-3,"y":"é\n"}}"#.utf8))
        XCTAssertEqual(value.object?.keys, ["b", "a", "c"])
        XCTAssertEqual(value["a"]?[0], .bool(true))
        XCTAssertEqual(value["a"]?[1], .null)
        XCTAssertEqual(value["a"]?[3], .double(2.5))
        XCTAssertEqual(value["c"]?["z"], .int(-3))
        XCTAssertEqual(value["c"]?.string("y"), "é\n")
        XCTAssertEqual(value["c"]?.object?.keys, ["z", "y"])
    }

    func testStringAccessorFollowsAndroidOptStringFallbacks() throws {
        let value = try JSON.parse(Data(#"{"empty":"","nul":"null","n":5,"flag":true,"name":"session","obj":{"k":1}}"#.utf8))
        XCTAssertEqual(value.string("missing", "empty", "nul", "name"), "session")
        XCTAssertEqual(value.string("n"), "5")
        XCTAssertEqual(value.string("flag"), "true")
        XCTAssertEqual(value.string("obj"), #"{"k":1}"#)
        XCTAssertEqual(value.string("missing"), "")
    }

    func testNumericAndBooleanAccessorsMatchOrgJson() throws {
        let value = try JSON.parse(Data(#"{"t":"TRUE","f":"no","i":"42","d":3.9,"big":9007199254740993}"#.utf8))
        XCTAssertTrue(value.bool("t"))
        XCTAssertFalse(value.bool("f"))
        XCTAssertFalse(value.bool("missing"))
        XCTAssertTrue(value.bool("missing", default: true))
        XCTAssertEqual(value.int("i"), 42)
        XCTAssertEqual(value.int("d"), 3)
        XCTAssertEqual(value.int("missing"), 0)
        XCTAssertEqual(value.int64("big"), 9_007_199_254_740_993)
        XCTAssertEqual(value.double("d"), 3.9)
        XCTAssertTrue(value.double("missing").isNaN)
        XCTAssertEqual(value.double("missing", default: 0), 0)
        XCTAssertNil(value.number("i"))
        XCTAssertEqual(value.number("d"), 3.9)
    }

    func testSerializesCompactlyInKeyOrderAndRoundTrips() throws {
        var object = JSONObject()
        object["request_id"] = "r1"
        object["payload"] = .object(["text": "a \"quoted\"\u{1}", "n": 1])
        object["ok"] = true
        let data = try JSON.object(object).data()
        XCTAssertEqual(String(decoding: data, as: UTF8.self),
                       #"{"request_id":"r1","payload":{"text":"a \"quoted\"\u0001","n":1},"ok":true}"#)
        XCTAssertEqual(try JSON.parse(data), .object(object))
    }

    func testRejectsMalformedTrailingAndTooDeepInput() {
        for text in ["{", "[1,]", #"{"a":1}x"#, "01", #""\x""#, "nul", #"{"a" 1}"#] {
            XCTAssertThrowsError(try JSON.parse(Data(text.utf8)), text)
        }
        let deep = String(repeating: "[", count: 600) + String(repeating: "]", count: 600)
        XCTAssertThrowsError(try JSON.parse(Data(deep.utf8)))
    }

    func testDuplicateKeysKeepTheLastValueLikeOrgJson() throws {
        let value = try JSON.parse(Data(#"{"a":1,"a":2}"#.utf8))
        XCTAssertEqual(value["a"], .int(2))
        XCTAssertEqual(value.object?.keys, ["a"])
    }
}
