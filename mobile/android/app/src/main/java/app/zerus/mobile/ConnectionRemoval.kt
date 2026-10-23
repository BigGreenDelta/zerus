package app.zerus.mobile

/** Keep the durable connection available for another Forget until protected cleanup succeeds. */
internal object ConnectionRemoval {
    fun forget(connection:Connection,current:List<Connection>,eraseProtected:(Connection)->Unit,
        saveIndex:(List<Connection>)->Unit):List<Connection> {
        check(connection in current) { "The connection changed before it could be forgotten. Refresh and try again." }
        if(connection.protocolVersion==2) eraseProtected(connection)
        val next=current.filterNot { it.id==connection.id }
        // A failed save leaves the old row actionable. Protected cleanup must be idempotent.
        saveIndex(next)
        return next
    }
}
