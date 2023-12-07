/*
 * Copyright 2023 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "lua/lua_common.h"
#include "rspamd.h"
#include "stat_internal.h"
#include "upstream.h"
#include "libserver/mempool_vars_internal.h"
#include "fmt/core.h"

#include <string>
#include <cstdint>
#include <vector>

#define msg_debug_stat_redis(...) rspamd_conditional_debug_fast(nullptr, nullptr,                                                 \
																rspamd_stat_redis_log_id, "stat_redis", task->task_pool->tag.uid, \
																RSPAMD_LOG_FUNC,                                                  \
																__VA_ARGS__)

INIT_LOG_MODULE(stat_redis)

#define REDIS_CTX(p) (reinterpret_cast<struct redis_stat_ctx *>(p))
#define REDIS_RUNTIME(p) (reinterpret_cast<struct redis_stat_runtime<float> *>(p))
#define REDIS_DEFAULT_OBJECT "%s%l"
#define REDIS_DEFAULT_USERS_OBJECT "%s%l%r"
#define REDIS_DEFAULT_TIMEOUT 0.5
#define REDIS_STAT_TIMEOUT 30
#define REDIS_MAX_USERS 1000

struct redis_stat_ctx {
	lua_State *L;
	struct rspamd_statfile_config *stcf;
	const char *redis_object = REDIS_DEFAULT_OBJECT;
	bool enable_users = false;
	bool store_tokens = false;
	bool enable_signatures = false;
	unsigned expiry;
	unsigned max_users = REDIS_MAX_USERS;
	int cbref_user = -1;

	int cbref_classify = -1;
	int cbref_learn = -1;
	int conf_ref = -1;
};


template<class T, std::enable_if_t<std::is_convertible_v<T, float>, bool> = true>
struct redis_stat_runtime {
	struct redis_stat_ctx *ctx;
	struct rspamd_task *task;
	struct rspamd_statfile_config *stcf;
	GPtrArray *tokens = nullptr;
	const char *redis_object_expanded;
	std::uint64_t learned = 0;
	int id;
	std::vector<std::pair<int, T>> *results = nullptr;
	bool need_redis_call = true;

	using result_type = std::vector<std::pair<int, T>>;

private:
	/* Called on connection termination */
	static void rt_dtor(gpointer data)
	{
		auto *rt = REDIS_RUNTIME(data);

		delete rt;
	}

	/* Avoid occasional deletion */
	~redis_stat_runtime()
	{
		if (tokens) {
			g_ptr_array_unref(tokens);
		}

		delete results;
	}

public:
	explicit redis_stat_runtime(struct redis_stat_ctx *_ctx, struct rspamd_task *_task, const char *_redis_object_expanded)
		: ctx(_ctx), task(_task), stcf(_ctx->stcf), redis_object_expanded(_redis_object_expanded)
	{
		rspamd_mempool_add_destructor(task->task_pool, redis_stat_runtime<T>::rt_dtor, this);
	}

	static auto maybe_recover_from_mempool(struct rspamd_task *task, const char *redis_object_expanded,
										   bool is_spam) -> std::optional<redis_stat_runtime<T> *>
	{
		auto var_name = fmt::format("{}_{}", redis_object_expanded, is_spam ? "S" : "H");
		auto *res = rspamd_mempool_get_variable(task->task_pool, var_name.c_str());

		if (res) {
			msg_debug_bayes("recovered runtime from mempool at %s", var_name.c_str());
			return reinterpret_cast<redis_stat_runtime<T> *>(res);
		}
		else {
			msg_debug_bayes("no runtime at %s", var_name.c_str());
			return std::nullopt;
		}
	}

	void set_results(std::vector<std::pair<int, T>> *results)
	{
		this->results = results;
	}

	/* Propagate results from internal representation to the tokens array */
	auto process_tokens(GPtrArray *tokens) const -> bool
	{
		rspamd_token_t *tok;

		if (!results) {
			return false;
		}

		for (auto [idx, val]: *results) {
			tok = (rspamd_token_t *) g_ptr_array_index(tokens, idx);
			tok->values[id] = val;
		}

		return true;
	}

	auto save_in_mempool(bool is_spam) const
	{
		auto var_name = fmt::format("{}_{}", redis_object_expanded, is_spam ? "S" : "H");
		/* We do not set destructor for the variable, as it should be already added on creation */
		rspamd_mempool_set_variable(task->task_pool, var_name.c_str(), (gpointer) this, nullptr);
		msg_debug_bayes("saved runtime in mempool at %s", var_name.c_str());
	}
};

#define GET_TASK_ELT(task, elt) (task == nullptr ? nullptr : (task)->elt)

static const gchar *M = "redis statistics";

static GQuark
rspamd_redis_stat_quark(void)
{
	return g_quark_from_static_string(M);
}

static inline struct upstream_list *
rspamd_redis_get_servers(struct redis_stat_ctx *ctx,
						 const gchar *what)
{
	lua_State *L = ctx->L;
	struct upstream_list *res;

	lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->conf_ref);
	lua_pushstring(L, what);
	lua_gettable(L, -2);
	res = *((struct upstream_list **) lua_touserdata(L, -1));
	lua_settop(L, 0);

	return res;
}

/*
 * Non-static for lua unit testing
 */
gsize rspamd_redis_expand_object(const gchar *pattern,
								 struct redis_stat_ctx *ctx,
								 struct rspamd_task *task,
								 gchar **target)
{
	gsize tlen = 0;
	const gchar *p = pattern, *elt;
	gchar *d, *end;
	enum {
		just_char,
		percent_char,
		mod_char
	} state = just_char;
	struct rspamd_statfile_config *stcf;
	lua_State *L = nullptr;
	struct rspamd_task **ptask;
	const gchar *rcpt = nullptr;
	gint err_idx;

	g_assert(ctx != nullptr);
	g_assert(task != nullptr);
	stcf = ctx->stcf;

	L = RSPAMD_LUA_CFG_STATE(task->cfg);
	g_assert(L != nullptr);

	if (ctx->enable_users) {
		if (ctx->cbref_user == -1) {
			rcpt = rspamd_task_get_principal_recipient(task);
		}
		else {
			/* Execute lua function to get userdata */
			lua_pushcfunction(L, &rspamd_lua_traceback);
			err_idx = lua_gettop(L);

			lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->cbref_user);
			ptask = (struct rspamd_task **) lua_newuserdata(L, sizeof(struct rspamd_task *));
			*ptask = task;
			rspamd_lua_setclass(L, "rspamd{task}", -1);

			if (lua_pcall(L, 1, 1, err_idx) != 0) {
				msg_err_task("call to user extraction script failed: %s",
							 lua_tostring(L, -1));
			}
			else {
				rcpt = rspamd_mempool_strdup(task->task_pool, lua_tostring(L, -1));
			}

			/* Result + error function */
			lua_settop(L, err_idx - 1);
		}

		if (rcpt) {
			rspamd_mempool_set_variable(task->task_pool, "stat_user",
										(gpointer) rcpt, nullptr);
		}
	}

	/* Length calculation */
	while (*p) {
		switch (state) {
		case just_char:
			if (*p == '%') {
				state = percent_char;
			}
			else {
				tlen++;
			}
			p++;
			break;
		case percent_char:
			switch (*p) {
			case '%':
				tlen++;
				state = just_char;
				break;
			case 'u':
				elt = GET_TASK_ELT(task, auth_user);
				if (elt) {
					tlen += strlen(elt);
				}
				break;
			case 'r':

				if (rcpt == nullptr) {
					elt = rspamd_task_get_principal_recipient(task);
				}
				else {
					elt = rcpt;
				}

				if (elt) {
					tlen += strlen(elt);
				}
				break;
			case 'l':
				if (stcf->label) {
					tlen += strlen(stcf->label);
				}
				/* Label miss is OK */
				break;
			case 's':
				tlen += sizeof("RS") - 1;
				break;
			default:
				state = just_char;
				tlen++;
				break;
			}

			if (state == percent_char) {
				state = mod_char;
			}
			p++;
			break;

		case mod_char:
			switch (*p) {
			case 'd':
				p++;
				state = just_char;
				break;
			default:
				state = just_char;
				break;
			}
			break;
		}
	}


	if (target == nullptr) {
		return -1;
	}

	*target = (gchar *) rspamd_mempool_alloc(task->task_pool, tlen + 1);
	d = *target;
	end = d + tlen + 1;
	d[tlen] = '\0';
	p = pattern;
	state = just_char;

	/* Expand string */
	while (*p && d < end) {
		switch (state) {
		case just_char:
			if (*p == '%') {
				state = percent_char;
			}
			else {
				*d++ = *p;
			}
			p++;
			break;
		case percent_char:
			switch (*p) {
			case '%':
				*d++ = *p;
				state = just_char;
				break;
			case 'u':
				elt = GET_TASK_ELT(task, auth_user);
				if (elt) {
					d += rspamd_strlcpy(d, elt, end - d);
				}
				break;
			case 'r':
				if (rcpt == nullptr) {
					elt = rspamd_task_get_principal_recipient(task);
				}
				else {
					elt = rcpt;
				}

				if (elt) {
					d += rspamd_strlcpy(d, elt, end - d);
				}
				break;
			case 'l':
				if (stcf->label) {
					d += rspamd_strlcpy(d, stcf->label, end - d);
				}
				break;
			case 's':
				d += rspamd_strlcpy(d, "RS", end - d);
				break;
			default:
				state = just_char;
				*d++ = *p;
				break;
			}

			if (state == percent_char) {
				state = mod_char;
			}
			p++;
			break;

		case mod_char:
			switch (*p) {
			case 'd':
				/* TODO: not supported yet */
				p++;
				state = just_char;
				break;
			default:
				state = just_char;
				break;
			}
			break;
		}
	}

	return tlen;
}

static int
rspamd_redis_stat_cb(lua_State *L)
{
	const auto *cookie = lua_tostring(L, lua_upvalueindex(1));
	auto *cfg = lua_check_config(L, 1);
	auto *backend = REDIS_CTX(rspamd_mempool_get_variable(cfg->cfg_pool, cookie));

	if (backend == nullptr) {
		msg_err("internal error: cookie %s is not found", cookie);

		return 0;
	}

	return 0;
}

static bool
rspamd_redis_parse_classifier_opts(struct redis_stat_ctx *backend,
								   const ucl_object_t *statfile_obj,
								   const ucl_object_t *classifier_obj,
								   struct rspamd_config *cfg)
{
	const gchar *lua_script;
	const ucl_object_t *elt, *users_enabled;
	auto *L = RSPAMD_LUA_CFG_STATE(cfg);

	users_enabled = ucl_object_lookup_any(classifier_obj, "per_user",
										  "users_enabled", nullptr);

	if (users_enabled != nullptr) {
		if (ucl_object_type(users_enabled) == UCL_BOOLEAN) {
			backend->enable_users = ucl_object_toboolean(users_enabled);
			backend->cbref_user = -1;
		}
		else if (ucl_object_type(users_enabled) == UCL_STRING) {
			lua_script = ucl_object_tostring(users_enabled);

			if (luaL_dostring(L, lua_script) != 0) {
				msg_err_config("cannot execute lua script for users "
							   "extraction: %s",
							   lua_tostring(L, -1));
			}
			else {
				if (lua_type(L, -1) == LUA_TFUNCTION) {
					backend->enable_users = TRUE;
					backend->cbref_user = luaL_ref(L,
												   LUA_REGISTRYINDEX);
				}
				else {
					msg_err_config("lua script must return "
								   "function(task) and not %s",
								   lua_typename(L, lua_type(L, -1)));
				}
			}
		}
	}
	else {
		backend->enable_users = FALSE;
		backend->cbref_user = -1;
	}

	elt = ucl_object_lookup(classifier_obj, "prefix");
	if (elt == nullptr || ucl_object_type(elt) != UCL_STRING) {
		/* Default non-users statistics */
		if (backend->enable_users || backend->cbref_user != -1) {
			backend->redis_object = REDIS_DEFAULT_USERS_OBJECT;
		}
		else {
			backend->redis_object = REDIS_DEFAULT_OBJECT;
		}
	}
	else {
		/* XXX: sanity check */
		backend->redis_object = ucl_object_tostring(elt);
	}

	elt = ucl_object_lookup(classifier_obj, "store_tokens");
	if (elt) {
		backend->store_tokens = ucl_object_toboolean(elt);
	}
	else {
		backend->store_tokens = FALSE;
	}

	elt = ucl_object_lookup(classifier_obj, "signatures");
	if (elt) {
		backend->enable_signatures = ucl_object_toboolean(elt);
	}
	else {
		backend->enable_signatures = FALSE;
	}

	elt = ucl_object_lookup_any(classifier_obj, "expiry", "expire", nullptr);
	if (elt) {
		backend->expiry = ucl_object_toint(elt);
	}
	else {
		backend->expiry = 0;
	}

	elt = ucl_object_lookup(classifier_obj, "max_users");
	if (elt) {
		backend->max_users = ucl_object_toint(elt);
	}
	else {
		backend->max_users = REDIS_MAX_USERS;
	}

	lua_pushcfunction(L, &rspamd_lua_traceback);
	auto err_idx = lua_gettop(L);

	/* Obtain function */
	if (!rspamd_lua_require_function(L, "lua_bayes_redis", "lua_bayes_init_classifier")) {
		msg_err_config("cannot require lua_bayes_redis.lua_bayes_init_classifier");
		lua_settop(L, err_idx - 1);

		return false;
	}

	/* Push arguments */
	ucl_object_push_lua(L, classifier_obj, false);
	ucl_object_push_lua(L, statfile_obj, false);
	lua_pushstring(L, backend->stcf->symbol);

	/* Store backend in random cookie */
	char *cookie = (char *) rspamd_mempool_alloc(cfg->cfg_pool, 16);
	rspamd_random_hex(cookie, 16);
	cookie[15] = '\0';
	rspamd_mempool_set_variable(cfg->cfg_pool, cookie, backend, nullptr);
	/* Callback */
	lua_pushstring(L, cookie);
	lua_pushcclosure(L, &rspamd_redis_stat_cb, 1);

	if (lua_pcall(L, 4, 2, err_idx) != 0) {
		msg_err("call to lua_bayes_init_classifier "
				"script failed: %s",
				lua_tostring(L, -1));
		lua_settop(L, err_idx - 1);

		return false;
	}

	/* Results are in the stack:
	 * top - 1 - classifier function (idx = -2)
	 * top - learn function (idx = -1)
	 */

	lua_pushvalue(L, -2);
	backend->cbref_classify = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_pushvalue(L, -1);
	backend->cbref_learn = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_settop(L, err_idx - 1);

	return true;
}

gpointer
rspamd_redis_init(struct rspamd_stat_ctx *ctx,
				  struct rspamd_config *cfg, struct rspamd_statfile *st)
{
	gint conf_ref = -1;
	auto *L = (lua_State *) cfg->lua_state;

	auto *backend = g_new0(struct redis_stat_ctx, 1);
	backend->L = L;
	backend->max_users = REDIS_MAX_USERS;

	backend->conf_ref = conf_ref;

	lua_settop(L, 0);

	if (!rspamd_redis_parse_classifier_opts(backend, st->stcf->opts, st->classifier->cfg->opts, cfg)) {
		msg_err_config("cannot init redis backend for %s", st->stcf->symbol);
		g_free(backend);
		return nullptr;
	}

	st->stcf->clcf->flags |= RSPAMD_FLAG_CLASSIFIER_INCREMENTING_BACKEND;
	backend->stcf = st->stcf;

#if 0
	backend->stat_elt = rspamd_stat_ctx_register_async(
		rspamd_redis_async_stat_cb,
		rspamd_redis_async_stat_fin,
		st_elt,
		REDIS_STAT_TIMEOUT);
	st_elt->async = backend->stat_elt;
#endif

	return (gpointer) backend;
}

gpointer
rspamd_redis_runtime(struct rspamd_task *task,
					 struct rspamd_statfile_config *stcf,
					 gboolean learn, gpointer c, gint _id)
{
	struct redis_stat_ctx *ctx = REDIS_CTX(c);
	char *object_expanded = nullptr;

	g_assert(ctx != nullptr);
	g_assert(stcf != nullptr);

	if (rspamd_redis_expand_object(ctx->redis_object, ctx, task,
								   &object_expanded) == 0) {
		msg_err_task("expansion for %s failed for symbol %s "
					 "(maybe learning per user classifier with no user or recipient)",
					 learn ? "learning" : "classifying",
					 stcf->symbol);
		return nullptr;
	}

	/* Look for the cached results */
	if (!learn) {
		auto maybe_existing = redis_stat_runtime<float>::maybe_recover_from_mempool(task,
																					object_expanded, stcf->is_spam);

		if (maybe_existing) {
			/* Update stcf to correspond to what we have been asked */
			maybe_existing.value()->stcf = stcf;
			return maybe_existing.value();
		}
	}

	/* No cached result, create new one */
	auto *rt = new redis_stat_runtime<float>(ctx, task, object_expanded);

	if (!learn) {
		/*
		 * For check, we also need to create the opposite class runtime to avoid
		 * double call for Redis scripts.
		 * This runtime will be filled later.
		 */
		auto maybe_opposite_rt = redis_stat_runtime<float>::maybe_recover_from_mempool(task,
																					   object_expanded,
																					   !stcf->is_spam);

		if (!maybe_opposite_rt) {
			auto *opposite_rt = new redis_stat_runtime<float>(ctx, task, object_expanded);
			opposite_rt->save_in_mempool(!stcf->is_spam);
			opposite_rt->need_redis_call = false;
		}
	}

	rt->save_in_mempool(stcf->is_spam);

	return rt;
}

void rspamd_redis_close(gpointer p)
{
	struct redis_stat_ctx *ctx = REDIS_CTX(p);
	lua_State *L = ctx->L;

	if (ctx->conf_ref) {
		luaL_unref(L, LUA_REGISTRYINDEX, ctx->conf_ref);
	}

	if (ctx->cbref_learn) {
		luaL_unref(L, LUA_REGISTRYINDEX, ctx->cbref_learn);
	}

	if (ctx->cbref_classify) {
		luaL_unref(L, LUA_REGISTRYINDEX, ctx->cbref_classify);
	}

	g_free(ctx);
}

/*
 * Serialise stat tokens to message pack
 */
static char *
rspamd_redis_serialize_tokens(struct rspamd_task *task, GPtrArray *tokens, gsize *ser_len)
{
	/* Each token is int64_t that requires 9 bytes + 4 bytes array len + 1 byte array magic */
	gsize req_len = tokens->len * 9 + 5, i;
	gchar *buf, *p;
	rspamd_token_t *tok;

	buf = (gchar *) rspamd_mempool_alloc(task->task_pool, req_len);
	p = buf;

	/* Array */
	*p++ = (gchar) 0xdd;
	/* Length in big-endian (4 bytes) */
	*p++ = (gchar) ((tokens->len >> 24) & 0xff);
	*p++ = (gchar) ((tokens->len >> 16) & 0xff);
	*p++ = (gchar) ((tokens->len >> 8) & 0xff);
	*p++ = (gchar) (tokens->len & 0xff);

	PTR_ARRAY_FOREACH(tokens, i, tok)
	{
		*p++ = (gchar) 0xd3;

		guint64 val = GUINT64_TO_BE(tok->data);
		memcpy(p, &val, sizeof(val));
		p += sizeof(val);
	}

	*ser_len = p - buf;

	return buf;
}

static gint
rspamd_redis_classified(lua_State *L)
{
	const auto *cookie = lua_tostring(L, lua_upvalueindex(1));
	auto *task = lua_check_task(L, 1);
	auto *rt = REDIS_RUNTIME(rspamd_mempool_get_variable(task->task_pool, cookie));
	/* TODO: write it */

	if (rt == nullptr) {
		msg_err_task("internal error: cannot find runtime for cookie %s", cookie);

		return 0;
	}

	bool result = lua_toboolean(L, 2);

	if (result) {
		/* Indexes:
		 * 3 - learned_ham (int)
		 * 4 - learned_spam (int)
		 * 5 - ham_tokens (pair<int, int>)
		 * 6 - spam_tokens (pair<int, int>)
		 */

		/*
		 * We need to fill our runtime AND the opposite runtime
		 */
		auto filler_func = [](redis_stat_runtime<float> *rt, lua_State *L, unsigned learned, int tokens_pos) {
			rt->learned = learned;
			redis_stat_runtime<float>::result_type *res;

			res = new redis_stat_runtime<float>::result_type(lua_objlen(L, tokens_pos));

			for (lua_pushnil(L); lua_next(L, tokens_pos); lua_pop(L, 1)) {
				lua_rawgeti(L, -1, 1);
				auto idx = lua_tointeger(L, -1);
				lua_pop(L, 1);

				lua_rawgeti(L, -1, 2);
				auto value = lua_tonumber(L, -1);
				lua_pop(L, 1);

				res->emplace_back(idx, value);
			}

			rt->set_results(res);
		};

		auto opposite_rt_maybe = redis_stat_runtime<float>::maybe_recover_from_mempool(task,
																					   rt->redis_object_expanded,
																					   !rt->stcf->is_spam);

		if (!opposite_rt_maybe) {
			msg_err_task("internal error: cannot find opposite runtime for cookie %s", cookie);

			return 0;
		}

		if (rt->stcf->is_spam) {
			filler_func(rt, L, lua_tointeger(L, 4), 6);
			filler_func(opposite_rt_maybe.value(), L, lua_tointeger(L, 3), 5);
		}
		else {
			filler_func(rt, L, lua_tointeger(L, 3), 5);
			filler_func(opposite_rt_maybe.value(), L, lua_tointeger(L, 4), 6);
		}

		/* Process all tokens */
		g_assert(rt->tokens != nullptr);
		rt->process_tokens(rt->tokens);
		opposite_rt_maybe.value()->process_tokens(rt->tokens);
	}
	else {
		/* Error message is on index 3 */
		msg_err_task("cannot classify task: %s",
					 lua_tostring(L, 3));
	}

	return 0;
}

gboolean
rspamd_redis_process_tokens(struct rspamd_task *task,
							GPtrArray *tokens,
							gint id, gpointer p)
{
	auto *rt = REDIS_RUNTIME(p);
	auto *L = rt->ctx->L;

	if (rspamd_session_blocked(task->s)) {
		return FALSE;
	}

	if (tokens == nullptr || tokens->len == 0) {
		return FALSE;
	}

	if (!rt->need_redis_call) {
		/* No need to do anything, as it is already done in the opposite class processing */

		return TRUE;
	}

	gsize tokens_len;
	gchar *tokens_buf = rspamd_redis_serialize_tokens(task, tokens, &tokens_len);

	rt->id = id;

	lua_pushcfunction(L, &rspamd_lua_traceback);
	gint err_idx = lua_gettop(L);

	/* Function arguments */
	lua_rawgeti(L, LUA_REGISTRYINDEX, rt->ctx->cbref_classify);
	rspamd_lua_task_push(L, task);
	lua_pushstring(L, rt->redis_object_expanded);
	lua_pushinteger(L, id);
	lua_pushboolean(L, rt->stcf->is_spam);
	lua_new_text(L, tokens_buf, tokens_len, false);

	/* Store rt in random cookie */
	char *cookie = (char *) rspamd_mempool_alloc(task->task_pool, 16);
	rspamd_random_hex(cookie, 16);
	cookie[15] = '\0';
	rspamd_mempool_set_variable(task->task_pool, cookie, rt, nullptr);
	/* Callback */
	lua_pushstring(L, cookie);
	lua_pushcclosure(L, &rspamd_redis_classified, 1);

	if (lua_pcall(L, 6, 0, err_idx) != 0) {
		msg_err_task("call to redis failed: %s", lua_tostring(L, -1));
		lua_settop(L, err_idx - 1);
		return FALSE;
	}

	rt->tokens = g_ptr_array_ref(tokens);

	lua_settop(L, err_idx - 1);
	return TRUE;
}

gboolean
rspamd_redis_finalize_process(struct rspamd_task *task, gpointer runtime,
							  gpointer ctx)
{
	return TRUE;
}

gboolean
rspamd_redis_learn_tokens(struct rspamd_task *task, GPtrArray *tokens,
						  gint id, gpointer p)
{
	auto *rt = REDIS_RUNTIME(p);

	/* TODO: write learn function */

	return FALSE;
}


gboolean
rspamd_redis_finalize_learn(struct rspamd_task *task, gpointer runtime,
							gpointer ctx, GError **err)
{
	return TRUE;
}

gulong
rspamd_redis_total_learns(struct rspamd_task *task, gpointer runtime,
						  gpointer ctx)
{
	auto *rt = REDIS_RUNTIME(runtime);

	return rt->learned;
}

gulong
rspamd_redis_inc_learns(struct rspamd_task *task, gpointer runtime,
						gpointer ctx)
{
	auto *rt = REDIS_RUNTIME(runtime);

	/* XXX: may cause races */
	return rt->learned + 1;
}

gulong
rspamd_redis_dec_learns(struct rspamd_task *task, gpointer runtime,
						gpointer ctx)
{
	auto *rt = REDIS_RUNTIME(runtime);

	/* XXX: may cause races */
	return rt->learned + 1;
}

gulong
rspamd_redis_learns(struct rspamd_task *task, gpointer runtime,
					gpointer ctx)
{
	auto *rt = REDIS_RUNTIME(runtime);

	return rt->learned;
}

ucl_object_t *
rspamd_redis_get_stat(gpointer runtime,
					  gpointer ctx)
{
	auto *rt = REDIS_RUNTIME(runtime);
	struct rspamd_redis_stat_elt *st;
	redisAsyncContext *redis;

	/* TODO: write extraction */

	return nullptr;
}

gpointer
rspamd_redis_load_tokenizer_config(gpointer runtime,
								   gsize *len)
{
	return nullptr;
}
