package app.zerus.mobile

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.time.LocalDate

class PrivateAnalyticsTest {
    private val machine = "10000000-0000-4000-8000-000000000001"
    private val epoch = "20000000-0000-4000-8000-000000000001"
    private val range = AnalyticsRange(LocalDate.parse("2026-10-01"), LocalDate.parse("2026-10-07"))
    private fun fixture(count: Long = 3) = JSONObject().put("schema", 1).put("machine_id", machine).put("source", "agent")
        .put("source_epoch", epoch).put("revision", 7).put("generated_at", 1_791_331_200.5)
        .put("range", JSONObject().put("from", "2026-10-01").put("through", "2026-10-07"))
        .put("collection", JSONObject().put("enabled", true).put("event_retention_days", 14).put("retention_days", 90))
        .put("coverage", JSONObject().put("start", 1_791_331_100).put("end", 1_791_331_200).put("gaps", JSONArray())
            .put("unsupported", JSONArray(listOf("cost"))).put("partial", true))
        .put("buckets", JSONArray().put(JSONObject().put("date", "2026-10-07").put("provider", "codex").put("producer", "main")
            .put("counters", JSONObject().put("turn_completed", count).put("work_ms", 1000))
            .put("histograms", JSONObject().put("work_ms", JSONObject().put("version", 1).put("bounds", JSONArray(PrivateAnalyticsProtocol.bounds))
                .put("counts", JSONArray(listOf(0, 1, 0, 0, 0, 0))).put("count", 1).put("sum", 1000)))))
    private fun parse(raw: JSONObject = fixture()) = PrivateAnalyticsProtocol.parse(raw, machine, range)

    private val resetEpoch = "20000000-0000-4000-8000-000000000002"
    private val otherRange = AnalyticsRange(range.from.minusDays(1), range.through)
    private fun response(epoch: String = this.epoch, revision: Long = 7, time: Double = 1_791_331_200.5,
        range: AnalyticsRange = this.range, count: Long = 3): JSONObject = fixture(count)
        .put("source_epoch", epoch).put("revision", revision).put("generated_at", time)
        .put("range", JSONObject().put("from", range.from.toString()).put("through", range.through.toString()))

    /** Execute the real controller refresh loop with only route discovery replaced by a private fixture. */
    private class ControllerFixture(scope: CoroutineScope) : AutoCloseable {
        val server = MockWebServer()
        var routeCount = 1
        var machineId = "10000000-0000-4000-8000-000000000001"
        private val replies = java.util.concurrent.LinkedBlockingQueue<JSONObject>()
        val controller: PrivateAnalyticsController
        init {
            server.start()
            server.dispatcher = object : okhttp3.mockwebserver.Dispatcher() {
                override fun dispatch(request: okhttp3.mockwebserver.RecordedRequest): MockResponse {
                    val reply = replies.poll(5, java.util.concurrent.TimeUnit.SECONDS)
                        ?: return MockResponse().setResponseCode(500)
                    val id = JSONObject(request.body.clone().readUtf8()).getString("request_id")
                    return MockResponse().setBody(JSONObject().put("request_id", id).put("state", "completed").put("result", reply).toString())
                }
            }
            controller = PrivateAnalyticsController(routes = {
                (0 until routeCount).map { index ->
                    val connection = Connection("fixture-$index", "Synthetic", server.url("/").toString(), "")
                    AnalyticsRoute(connection, MachineKey(connection.id, "computer"), machineId, "Synthetic", AnalyticsReadState.Ready, false)
                }
            }, routeCurrent = { _, _ -> true }, scope = scope, api = RelayApi())
        }
        suspend fun refresh(scope: CoroutineScope, range: AnalyticsRange, vararg responses: JSONObject): AnalyticsComputer {
            routeCount = responses.size
            responses.forEach { replies.add(it) }
            controller.refresh(range)
            scope.coroutineContext.job.children.toList().joinAll()
            assertFalse(controller.value.loading)
            assertEquals(1, controller.value.computers.size)
            assertTrue(replies.isEmpty())
            return controller.value.computers.single()
        }
        override fun close() { controller.stop(); server.close() }
    }

    @Test fun rejectedRepliesNeverPromotePreviousRefreshToCurrent() = runBlocking {
        ControllerFixture(this).use { f ->
            f.refresh(this, range, response())
            val accepted = f.refresh(this, range, response(resetEpoch, 10, 1_791_331_202.5, count = 0))
            assertEquals(AnalyticsReadState.Current, accepted.state)
            val rejected = listOf(
                response(time = 1_791_331_203.5), // Retired source even with newer route time.
                response(resetEpoch, 9, 1_791_331_203.5), // Lower revision despite newer time.
                response(resetEpoch, 11, 1_791_331_201.5), // Older generation time.
                response("20000000-0000-4000-8000-000000000003", 0, 1_791_331_202.5)) // Ambiguous reset.
            rejected.forEach { reply ->
                val kept = f.refresh(this, range, reply)
                assertEquals(accepted.snapshot, kept.snapshot)
                assertEquals(AnalyticsReadState.Invalid, kept.state)
                val diagnostic = JSONObject(PrivateAnalyticsDiagnostics.preview(f.controller.value, 42, 1_791_331_203.5))
                assertTrue(diagnostic.getJSONArray("computers").getJSONObject(0).getBoolean("stale"))
            }
        }
    }

    @Test fun acceptedRouteWithinRefreshStaysCurrentAfterRejectedRoute() = runBlocking {
        ControllerFixture(this).use { f ->
            f.refresh(this, range, response())
            val current = f.refresh(this, range,
                response(resetEpoch, 0, 1_791_331_202.5, count = 0), response(time = 1_791_331_203.5))
            assertEquals(AnalyticsReadState.Current, current.state)
            assertEquals(resetEpoch, current.snapshot!!.epoch)
            assertEquals(0L, current.snapshot.buckets.single().counters["turn_completed"])
            val recovered = f.refresh(this, range, response(time = 1_791_331_204.5),
                response(resetEpoch, 1, 1_791_331_205.5, count = 1))
            assertEquals(AnalyticsReadState.Current, recovered.state)
            assertEquals(1L, recovered.snapshot!!.revision)
        }
    }

    @Test fun rangeSwitchRetainsRetiredEpochGuardAndDropsIncompatibleBuckets() = runBlocking {
        ControllerFixture(this).use { f ->
            f.refresh(this, range, response())
            f.refresh(this, range, response(resetEpoch, 0, 1_791_331_202.5, count = 0))
            val rejected = f.refresh(this, otherRange, response(time = 1_791_331_203.5, range = otherRange))
            assertEquals(AnalyticsReadState.Invalid, rejected.state)
            assertNull(rejected.snapshot)
            assertTrue(projectPrivateAnalytics(f.controller.value.computers, "", "", "main").counters.isEmpty())
            val fresh = f.refresh(this, otherRange, response(resetEpoch, 1, 1_791_331_204.5, otherRange, 2))
            assertEquals(AnalyticsReadState.Current, fresh.state)
            assertEquals(otherRange, fresh.snapshot!!.range)
            assertEquals(2L, fresh.snapshot.buckets.single().counters["turn_completed"])
            val back = f.refresh(this, range, response(time = 1_791_331_205.5))
            assertEquals(AnalyticsReadState.Invalid, back.state)
            assertNull(back.snapshot)
        }
    }

    @Test fun rangeSwitchDoesNotBypassSourceRevisionOrTimeGuard() = runBlocking {
        ControllerFixture(this).use { f ->
            f.refresh(this, range, response(revision = 10, time = 1_791_331_202.5))
            val rejected = listOf(response(revision = 9, time = 1_791_331_203.5, range = otherRange),
                response(revision = 11, time = 1_791_331_201.5, range = otherRange))
            rejected.forEach { raw ->
                val kept = f.refresh(this, otherRange, raw)
                assertEquals(AnalyticsReadState.Invalid, kept.state)
                assertNull(kept.snapshot)
            }
            val current = f.refresh(this, otherRange, response(revision = 10, time = 1_791_331_203.5, range = otherRange))
            assertEquals(AnalyticsReadState.Current, current.state)
            assertEquals(otherRange, current.snapshot!!.range)
        }
    }

    @Test fun fleetReplacementChecksRetirementBeforeRange() {
        val original = parse()
        val reset = AnalyticsFleet.replace(original, original.copy(epoch = resetEpoch, revision = 0,
            generatedAt = original.generatedAt + 1, buckets = emptyList()))
        assertEquals(reset, AnalyticsFleet.replace(reset, original.copy(range = otherRange, generatedAt = original.generatedAt + 2)))
        val fresh = AnalyticsFleet.replace(reset, reset.copy(range = otherRange, generatedAt = original.generatedAt + 3))
        assertEquals(otherRange, fresh.range)
        assertEquals(setOf(epoch), fresh.retiredEpochs)
    }

    @Test fun emptyHistogramRejectsNonzeroSumBeforePooling() {
        val known = parse()
        val secondMachine = "10000000-0000-4000-8000-000000000002"
        val raw = fixture().put("machine_id", secondMachine)
        val histogram = raw.getJSONArray("buckets").getJSONObject(0).getJSONObject("histograms").getJSONObject("work_ms")
        histogram.put("counts", JSONArray(List(6) { 0 })).put("count", 0).put("sum", 900)
        val accepted = runCatching { PrivateAnalyticsProtocol.parse(raw, secondMachine, range) }.getOrNull()
        val pooled = AnalyticsFleet.workHistogram(AnalyticsFleet.buckets(listOfNotNull(known, accepted)))!!
        assertEquals(1_000.0, pooled.meanMs!!, 0.001)
        assertNull(accepted)
        histogram.put("sum", 0)
        val validEmpty = PrivateAnalyticsProtocol.parse(raw, secondMachine, range)
        assertNull(validEmpty.buckets.single().workHistogram!!.meanMs)
        assertEquals(known.buckets.single().workHistogram,
            AnalyticsFleet.workHistogram(AnalyticsFleet.buckets(listOf(known, validEmpty))))
    }

    @Test fun retiredEpochBoundNeverEvictsHistoryAcrossRanges() {
        var accepted = parse()
        repeat(PrivateAnalyticsProtocol.maxRetiredEpochs) { index ->
            accepted = AnalyticsFleet.replace(accepted, accepted.copy(
                epoch = java.util.UUID(2L, index.toLong() + 2).toString(), revision = 0,
                generatedAt = accepted.generatedAt + 1, range = if (index % 2 == 0) otherRange else range))
        }
        assertEquals(64, accepted.retiredEpochs.size)
        val full = accepted
        assertThrows(IllegalArgumentException::class.java) {
            AnalyticsFleet.replace(full, full.copy(epoch = java.util.UUID(2L, 100L).toString(), generatedAt = full.generatedAt + 1))
        }
        assertEquals(full, AnalyticsFleet.replace(full, parse().copy(generatedAt = full.generatedAt + 2, range = otherRange)))
        val sameSource = AnalyticsFleet.replace(full, full.copy(revision = 1, generatedAt = full.generatedAt + 1, range = otherRange))
        assertEquals(1L, sameSource.revision)
        assertEquals(full.retiredEpochs, sameSource.retiredEpochs)
    }

    @Test fun controllerMachineGuardBoundRetainsDisappearedSources() = runBlocking {
        ControllerFixture(this).use { f ->
            f.refresh(this, range, response())
            f.refresh(this, range, response(resetEpoch, 0, 1_791_331_202.5, count = 0))
            repeat(PrivateAnalyticsProtocol.maxMachines - 1) { index ->
                f.machineId = java.util.UUID(1L, index.toLong() + 2).toString()
                val accepted = f.refresh(this, range, response().put("machine_id", f.machineId))
                assertEquals(AnalyticsReadState.Current, accepted.state)
            }
            f.machineId = java.util.UUID(1L, 100L).toString()
            val overflow = f.refresh(this, range, response().put("machine_id", f.machineId))
            assertEquals(AnalyticsReadState.Invalid, overflow.state)
            assertNull(overflow.snapshot)
            f.machineId = machine
            val retired = f.refresh(this, otherRange, response(time = 1_791_331_203.5, range = otherRange))
            assertEquals(AnalyticsReadState.Invalid, retired.state)
            assertNull(retired.snapshot)
            val current = f.refresh(this, otherRange, response(resetEpoch, 1, 1_791_331_204.5, otherRange))
            assertEquals(AnalyticsReadState.Current, current.state)
            assertEquals(setOf(epoch), current.snapshot!!.retiredEpochs)
        }
    }

    @Test fun readsRequireFreshMachineIdentityAndCapabilitiesOnBothEnds() {
        val supported = setOf("analytics_read")
        assertEquals(AnalyticsReadState.Ready, privateAnalyticsAccess(machine, true, true, supported, supported))
        assertEquals(AnalyticsReadState.Unsupported, privateAnalyticsAccess(machine, true, true, emptySet(), supported))
        assertEquals(AnalyticsReadState.Unsupported, privateAnalyticsAccess(machine, true, true, supported, emptySet()))
        assertEquals(AnalyticsReadState.Offline, privateAnalyticsAccess(machine, false, true, supported, supported))
        assertEquals(AnalyticsReadState.Offline, privateAnalyticsAccess(machine, true, false, supported, supported))
        assertEquals(AnalyticsReadState.Unverified, privateAnalyticsAccess("", true, true, supported, supported))
    }

    @Test fun boundedUtcRangeIsInclusive() {
        assertEquals("2026-07-10", AnalyticsRange.recent(90, LocalDate.parse("2026-10-07")).from.toString())
        assertThrows(IllegalArgumentException::class.java) { AnalyticsRange(range.from, range.from.minusDays(1)) }
        assertThrows(IllegalArgumentException::class.java) { AnalyticsRange(range.from, range.from.plusDays(90)) }
        assertEquals(3, range.json().length())
    }
    @Test fun absentStoreAndLocallyShortenedRetentionRemainReadable() {
        val raw = fixture().put("source_epoch", "00000000-0000-0000-0000-000000000000").put("buckets", JSONArray())
        raw.getJSONObject("collection").put("enabled", false).put("retention_days", 30)
        raw.getJSONObject("coverage").put("start", JSONObject.NULL).put("end", JSONObject.NULL)
            .put("providers", JSONObject().put("codex", JSONObject().put("unsupported", JSONArray(listOf("tokens", "private detail")))))
        raw.put("unresolved_waits", JSONObject().put("observed", 2).put("complete", false))
        val parsed = parse(raw)
        assertFalse(parsed.enabled); assertEquals(30, parsed.retentionDays); assertTrue(parsed.buckets.isEmpty())
        assertEquals(setOf("tokens", "unknown"), parsed.coverage.providerUnsupported["codex"])
        assertEquals(2L, parsed.unresolvedWaits)
        assertThrows(IllegalArgumentException::class.java) { parse(raw.also { it.getJSONObject("collection").put("retention_days", 91) }) }
    }
    @Test fun duplicateRoutesAndRepeatedPollsNeverMultiplyCounts() {
        val first = parse(); val next = parse(fixture(4).put("revision", 8).put("generated_at", first.generatedAt + 1))
        assertEquals(3L, AnalyticsFleet.totals(listOf(first, first))["turn_completed"])
        assertEquals(4L, AnalyticsFleet.totals(listOf(next, first, next))["turn_completed"])
        assertEquals(next, AnalyticsFleet.replace(next, first))
        val oldRevisionNewTime = first.copy(generatedAt = next.generatedAt + 1)
        assertEquals(next, AnalyticsFleet.replace(next, oldRevisionNewTime))
    }
    @Test fun dailyHistogramsMergeUnequalSamplesAfterRouteDeduplication() {
        val first = parse()
        val second = first.copy(machineId = "10000000-0000-4000-8000-000000000002",
            buckets = listOf(first.buckets.single().copy(counters = mapOf("turn_completed" to 4L),
                workHistogram = AnalyticsHistogram(listOf(0, 0, 9, 0, 0, 0), 9, 27_000))))
        val computers = listOf(first, first, second).map { AnalyticsComputer(it.machineId, "Synthetic", AnalyticsReadState.Current, it) }
        val projected = projectPrivateAnalytics(computers, "", "", "main")
        assertFalse(projected.overflow)
        assertEquals(7L, projected.counters["turn_completed"])
        assertEquals(projected.counters, projected.daily.single().counters)
        val histogram = projected.daily.single().workHistogram
        assertNotNull(histogram)
        assertEquals(listOf(0L, 1L, 9L, 0L, 0L, 0L), histogram!!.counts)
        assertEquals(10L, histogram.count)
        assertEquals(28_000L, histogram.sum)
        // Weight by ten observations, not the mean of machine means (2,000 ms).
        assertEquals(2_800.0, histogram.sum.toDouble() / histogram.count, 0.001)
        assertEquals(2_800.0, histogram.meanMs!!, 0.001)
        assertEquals(histogram, projected.workHistogram)
    }
    @Test fun missingHistogramDoesNotInventSamplesAndEmptyHistogramHasNoMean() {
        val known = parse()
        val missing = known.copy(machineId = "10000000-0000-4000-8000-000000000002",
            buckets = known.buckets.map { it.copy(workHistogram = null) })
        fun project(vararg snapshots: AnalyticsSnapshot) = projectPrivateAnalytics(snapshots.map {
            AnalyticsComputer(it.machineId, "Synthetic", AnalyticsReadState.Current, it)
        }, "", "", "main")
        assertNull(project(missing).workHistogram)
        assertEquals(known.buckets.single().workHistogram, project(known, missing).workHistogram)
        val empty = known.copy(buckets = known.buckets.map {
            it.copy(workHistogram = AnalyticsHistogram(List(6) { 0 }, 0, 0))
        })
        assertEquals(0L, project(empty).workHistogram!!.count)
        assertNull(project(empty).workHistogram!!.meanMs)
    }
    @Test fun histogramAndCounterMergeBoundsFailClosed() {
        val first = parse()
        val second = first.copy(machineId = "10000000-0000-4000-8000-000000000002")
        val max = PrivateAnalyticsProtocol.maxNumber
        val rows = listOf(
            first.buckets.single().copy(workHistogram = AnalyticsHistogram(listOf(max, 0, 0, 0, 0, 0), max, 0)),
            first.buckets.single().copy(workHistogram = AnalyticsHistogram(listOf(0, 1, 0, 0, 0, 0), 1, max)),
            first.buckets.single().copy(counters = mapOf("turn_completed" to max)))
        rows.forEach { row ->
            val computers = listOf(first.copy(buckets = listOf(row)), second).map {
                AnalyticsComputer(it.machineId, "Synthetic", AnalyticsReadState.Current, it)
            }
            val projection = projectPrivateAnalytics(computers, "", "", "main")
            assertTrue(projection.overflow)
            assertTrue(projection.counters.isEmpty()); assertTrue(projection.daily.isEmpty())
            assertNull(projection.workHistogram)
        }
    }
    @Test fun dailyProjectionUsesAcceptedResetAndSelectedProducerBeforeMerging() {
        val first = parse()
        val child = first.buckets.single().copy(producer = "child", counters = mapOf("turn_completed" to 12L),
            workHistogram = AnalyticsHistogram(listOf(0, 0, 9, 0, 0, 0), 9, 27_000))
        val updated = first.copy(revision = first.revision + 1, generatedAt = first.generatedAt + 1,
            buckets = listOf(first.buckets.single(), child))
        fun project(snapshots: List<AnalyticsSnapshot>, producer: String) = projectPrivateAnalytics(snapshots.map {
            AnalyticsComputer(it.machineId, "Synthetic", AnalyticsReadState.Current, it)
        }, "", "codex", producer)
        assertEquals(9L, project(listOf(first, updated, first), "child").workHistogram!!.count)
        assertEquals(1L, project(listOf(first, updated, first), "main").workHistogram!!.count)
        val purged = AnalyticsFleet.replace(updated, updated.copy(epoch = "20000000-0000-4000-8000-000000000002",
            revision = 0, generatedAt = updated.generatedAt + 1, buckets = emptyList()))
        val late = updated.copy(generatedAt = purged.generatedAt + 1)
        assertTrue(project(listOf(purged, late), "main").daily.isEmpty())
        assertNull(project(listOf(purged, late), "main").workHistogram)
    }
    @Test fun newEpochReplacesPurgedTotalsAndLateOldReplyCannotResurrectThem() {
        val original = parse()
        val purged = parse(fixture().put("source_epoch", "20000000-0000-4000-8000-000000000002")
            .put("revision", 0).put("generated_at", original.generatedAt + 1).put("buckets", JSONArray()))
        val accepted = AnalyticsFleet.replace(original, purged)
        assertEquals(purged.epoch, accepted.epoch)
        assertEquals(accepted, AnalyticsFleet.replace(accepted, original))
        assertTrue(AnalyticsFleet.totals(listOf(original, purged)).isEmpty())
        assertEquals(original, AnalyticsFleet.replace(original, purged.copy(generatedAt = original.generatedAt)))
    }
    @Test fun knownRetiredEpochCannotReturnWithANewerRouteTimestamp() {
        val original = parse()
        val purged = original.copy(epoch = "20000000-0000-4000-8000-000000000002", revision = 0,
            generatedAt = original.generatedAt + 1, buckets = emptyList())
        val accepted = AnalyticsFleet.replace(original, purged)
        val lateOldRoute = original.copy(generatedAt = original.generatedAt + 2)
        assertTrue(AnalyticsFleet.replace(accepted, lateOldRoute).buckets.isEmpty())
    }
    @Test fun unsupportedAndAbsentValuesStayUnknownAndChildrenStaySeparate() {
        val raw = fixture()
        val child = JSONObject(raw.getJSONArray("buckets").getJSONObject(0).toString()).put("producer", "child")
            .put("counters", JSONObject().put("turn_completed", 12))
        raw.getJSONArray("buckets").put(child)
        raw.getJSONObject("coverage").put("unsupported", JSONArray(listOf("tokens", "a private prompt")))
            .put("gaps", JSONArray(listOf("history_purged", "/synthetic/private/source")))
        val parsed = parse(raw)
        assertFalse(parsed.buckets.first().counters.containsKey("input_tokens"))
        assertEquals(setOf("tokens", "unknown"), parsed.coverage.unsupported)
        assertEquals(setOf("history_purged", "unknown"), parsed.coverage.gaps)
        assertEquals(3L, AnalyticsFleet.totals(listOf(parsed), producer = "main")["turn_completed"])
        assertEquals(12L, AnalyticsFleet.totals(listOf(parsed), producer = "child")["turn_completed"])
        assertTrue(AnalyticsFleet.stale(parsed, parsed.generatedAt + 301))
        assertTrue(AnalyticsFleet.stale(parsed, parsed.generatedAt - 61))
    }
    @Test fun exactMachineRangeSchemaDimensionsAndHistogramAreRequired() {
        val invalid = listOf<(JSONObject) -> Unit>(
            { it.put("machine_id", "10000000-0000-4000-8000-000000000002") },
            { it.put("source_epoch", "secret/path") }, { it.put("source", "speech") }, { it.put("schema", 2) },
            { it.getJSONObject("range").put("through", "2026-10-08") },
            { it.getJSONArray("buckets").getJSONObject(0).put("date", "2026-09-30") },
            { it.getJSONArray("buckets").getJSONObject(0).put("provider", "a private provider response") },
            { it.getJSONArray("buckets").getJSONObject(0).put("producer", "session-id") },
            { it.getJSONArray("buckets").put(it.getJSONArray("buckets").getJSONObject(0)) },
            { it.getJSONArray("buckets").getJSONObject(0).getJSONObject("histograms").getJSONObject("work_ms").put("count", 2) },
            { it.getJSONArray("buckets").getJSONObject(0).getJSONObject("histograms").getJSONObject("work_ms").put("version", 2) },
            { it.getJSONArray("buckets").getJSONObject(0).getJSONObject("histograms").getJSONObject("work_ms").put("bounds", JSONArray(listOf(100, 1000, 10000, 60000, 600001))) },
            { it.getJSONObject("collection").put("enabled", "true") },
            { it.getJSONObject("coverage").put("start", 1_800_000_000) })
        invalid.forEach { mutation -> assertThrows(Exception::class.java) { parse(fixture().also(mutation)) } }
        listOf<Any>(-1, 1.5, "5", true, PrivateAnalyticsProtocol.maxNumber + 1).forEach { number ->
            assertThrows(Exception::class.java) { parse(fixture().also { it.getJSONArray("buckets").getJSONObject(0).getJSONObject("counters").put("work_ms", number) }) }
        }
    }
    @Test fun rowAndByteBoundsFailClosed() {
        assertThrows(IllegalArgumentException::class.java) { parse(fixture().put("buckets", JSONArray(List(1081) { JSONObject() }))) }
        assertThrows(IllegalArgumentException::class.java) { parse(fixture().put("ignored", "x".repeat(PrivateAnalyticsProtocol.maxBytes))) }
    }
    @Test fun diagnosticsNeverEchoUnreviewedFieldsOrStableIdentities() {
        val raw = fixture().put("prompt", "SYNTHETIC_PRIVATE_PROMPT").put("path", "/synthetic/private/file")
        raw.getJSONArray("buckets").getJSONObject(0).getJSONObject("counters").put("secret", "SYNTHETIC_SECRET")
        val snapshot = parse(raw)
        val state = PrivateAnalyticsState(range, listOf(AnalyticsComputer(machine, "SYNTHETIC_PRIVATE_NAME", AnalyticsReadState.Current, snapshot)))
        val preview = PrivateAnalyticsDiagnostics.preview(state, 42, snapshot.generatedAt)
        listOf(machine, epoch, "SYNTHETIC", "/synthetic", "prompt", "source_epoch", "token", "path").forEach { assertFalse(it, preview.contains(it)) }
        val diagnostic = JSONObject(preview)
        assertEquals(42, diagnostic.getInt("android_build_code"))
        assertEquals(1, diagnostic.getJSONArray("computers").getJSONObject(0).getInt("bucket_count"))
    }
    @Test fun speechUsesDailyRowsOnlyWithinRangeAndDropsProfileIdentity() {
        fun day(date: String, count: Int) = JSONObject().put("day", date).put("profile_id", "SYNTHETIC_PRIVATE_PROFILE")
            .put("completed", count).put("audio_ms", 1001.75).put("processing_ms", 100.4)
            .put("error_codes", JSONObject().put("private error response", 1))
        val raw = JSONObject().put("schema", 1).put("enabled", false).put("retention_days", 90)
            .put("totals", JSONObject().put("completed", 9999))
            .put("days", JSONArray(listOf(day("2026-09-30", 50), day("2026-10-07", 2))))
        val speech = PrivateAnalyticsProtocol.speech(raw, range)
        assertFalse(speech.enabled); assertEquals(2L, speech.counters["completed"])
        assertEquals(1001L, speech.counters["audio_ms"]); assertEquals(mapOf("unknown" to 1L), speech.errorCodes)
        assertFalse(speech.toString().contains("PRIVATE_PROFILE"))
        assertFalse(speech.counters.containsKey("queue_ms"))
    }
    @Test fun computerReadUsesExactAuthorizedRouteAndOnlyReviewedPayload() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            server.dispatcher = object : okhttp3.mockwebserver.Dispatcher() {
                override fun dispatch(request: okhttp3.mockwebserver.RecordedRequest): MockResponse {
                    val body = JSONObject(request.body.clone().readUtf8())
                    return MockResponse().setBody(JSONObject().put("request_id", body.getString("request_id")).put("state", "completed").put("result", fixture()).toString())
                }
            }
            val route = Connection("workspace", "Synthetic", server.url("/").toString().trimEnd('/'), "synthetic-token")
            assertEquals(machine, RelayApi().analytics(route, MachineKey("workspace", "route-computer"), machine, range).machineId)
            assertEquals(1, server.requestCount)
            val request = server.takeRequest()
            assertEquals("/v1/requests", request.path)
            assertEquals("Bearer synthetic-token", request.getHeader("Authorization"))
            assertEquals("POST", request.method)
            val body = JSONObject(request.body.readUtf8())
            assertEquals("route-computer", body.getString("computer_id")); assertEquals("", body.getString("session"))
            assertEquals("analytics_read", body.getString("operation")); assertEquals(range.json().toString(), body.getJSONObject("payload").toString())
        }
    }
    @Test fun wrongReceiptIsRejectedWithoutRetry() = runBlocking {
        MockWebServer().use { server ->
            server.start(); server.enqueue(MockResponse().setBody(JSONObject().put("request_id", "different").put("state", "completed").put("result", fixture()).toString()))
            val route = Connection("workspace", "Synthetic", server.url("/").toString(), "")
            assertThrows(IllegalStateException::class.java) { runBlocking { RelayApi().analytics(route, MachineKey("workspace", "route-computer"), machine, range) } }
            assertEquals(1, server.requestCount)
        }
    }
}
