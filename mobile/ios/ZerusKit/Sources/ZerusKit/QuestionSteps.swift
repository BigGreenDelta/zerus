import Foundation

/// Where a multi-part answer stands. Navigation never submits a partially answered native request.
/// Ported from Android QuestionSteps.
public struct QuestionStep: Equatable, Sendable {
    public let page: Int
    public let count: Int
    /// The page the primary action moves to; nil when it sends.
    public let nextPage: Int?
    public let canAdvance: Bool
    public let complete: Bool

    /// The last page while an earlier answer is still missing or invalid: the action returns to it.
    public var reviewing: Bool { nextPage != nil && page == count - 1 }
}

public enum QuestionSteps {
    public static func current(_ prompts: [Prompt], page: Int, options: [String: Set<String>], texts: [String: String]) -> QuestionStep {
        let index = min(max(page, 0), max(prompts.count - 1, 0))
        let answered = prompts.map { QuestionPolicies.complete([$0], options: options, texts: texts) }
        let complete = !answered.isEmpty && answered.allSatisfy { $0 }
        let next: Int? = if index < prompts.count - 1 {
            index + 1
        } else if !complete, let missing = answered.firstIndex(of: false), missing < index {
            missing
        } else {
            nil
        }
        let current = answered.indices.contains(index) ? answered[index] : false
        return QuestionStep(page: index, count: prompts.count, nextPage: next, canAdvance: next != nil && (index == prompts.count - 1 || current),
                            complete: complete)
    }
}
