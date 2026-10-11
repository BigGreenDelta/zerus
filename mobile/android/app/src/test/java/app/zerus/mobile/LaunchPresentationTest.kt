package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class LaunchPresentationTest {
    @Test fun providersAndAccountsComeOnlyFromTypedNativeCatalog() {
        val raw = JSONObject().put("agents",JSONArray().put("codex").put("unknown").put(1).put("codex").put("kimi"))
            .put("accounts",JSONArray().put(JSONObject().put("id","safe-id").put("provider","codex").put("label","Work"))
                .put(JSONObject().put("id",1).put("provider","codex").put("label","Wrong type"))
                .put(JSONObject().put("id","other").put("provider","kimi").put("label","Other")))
        assertEquals(listOf("codex","kimi"),LaunchPresentation.agents(raw))
        assertEquals(listOf(LaunchAccount("safe-id","codex","Work")),LaunchPresentation.accounts(raw,"codex"))
    }
    @Test fun selectionMustStillBeInCurrentVerifiedBrowser() {
        val browser = JSONObject().put("path","/srv/projects").put("directories",JSONArray().put(JSONObject().put("path","/srv/projects/example").put("name","Example")))
        assertTrue(LaunchPresentation.selectedFolder(browser,"/srv/projects"))
        assertTrue(LaunchPresentation.selectedFolder(browser,"/srv/projects/example"))
        assertFalse(LaunchPresentation.selectedFolder(browser,"/srv/other"))
        assertFalse(LaunchPresentation.selectedFolder(browser,"~"))
        assertFalse(LaunchPresentation.absoluteFolder("/srv/new\nfolder"))
    }
    @Test fun nativeTagLimitsAreExplicitAndUtf8Bounded() {
        assertEquals("",LaunchPresentation.nameError("mobile launch"))
        assertNotEquals("",LaunchPresentation.nameError("../escape"))
        assertNotEquals("",LaunchPresentation.nameError(" example "))
        assertEquals("",LaunchPresentation.nameError("界".repeat(40)))
        assertNotEquals("",LaunchPresentation.nameError("界".repeat(41)))
    }
}
