var esrefactorContext = new esrefactor.Context();

function dumpObject(obj)
{
    for (var key in obj) {
        if (obj.hasOwnProperty(key) && typeof obj[key] !== 'function') {
            log(key);
        }
    }
}


function resolveName(node)
{
    if (node.type == "Identifier") {
        return node.name;
    } else if (node.type == "MemberExpression") {
        return resolveName(node.object) + "." + resolveName(node.property);
    } else if (node.type == "Literal") {
        return node.value;
    } else if (node.type == "ThisExpression") {
        log("got this here", node);
    } else {
        log("Got here", node);
    }
    return "";
}

function indexFile(code, file, cb)
{
    var parsed = esprima.parse(code, { range: true, tolerant: true });
    if (!parsed) {
        throw new Error("Couldn't parse file " + file + ' ' + code.length);
        return false;
    }
    esrefactorContext.setCode(parsed);
    if (!esrefactorContext._syntax)
        throw new Error('Unable to identify anything without a syntax tree');
    if (!esrefactorContext._scopeManager)
        throw new Error('Unable to identify anything without a valid scope manager');

    var scopeManager = esrefactorContext._scopeManager;
    var lookup = esrefactorContext._lookup;
    // log(JSON.stringify(esrefactorContext._syntax, null, 4));
    log(esrefactorContext._syntax);

    scopeManager.attach();
    var parents = [];
    function isChild(key, offset)
    {
        if (typeof offset == 'undefined')
            offset = parents.length - 1;
        return (offset > 0
                && parents[offset - 1]
                && typeof parents[offset - 1][key] != 'undefined'
                && offset <= parents.length
                && parents[offset - 1][key] == parents[offset]);
    }
    function parentTypeIs(type, offset)
    {
        if (typeof offset == 'undefined')
            offset = parents.length - 1;
        return (offset > 0 && parents[offset - 1] && parents[offset - 1].type == type);
    }

    var scopes = [];
    var scopeStack = [];

    function add(path, range, declaration) {
        if (path == "constructor")
            path = " constructor"; // heh
        var scope = scopeStack[scopeStack.length - 1];
        var cur = null;
        if (!declaration) {
            for (var i=scopeStack.length - 1; i>=0; --i) {
                cur = scopeStack[i].objects[path];
                if (cur) {
                    // log("Found", path, "in a scope", i, scopeStack.length);
                    scope = scopeStack[i];
                    break;
                }
            }
        }
        if (!cur) {
            range.push(true);
            scope.objects[path] = [range];
        } else {
            scope.objects[path].push(range);
        }
    }

    var byName = {};
    estraverse.traverse(esrefactorContext._syntax, {
        enter: function (node) {
            var path;
            parents.push(node);
            var s = scopeManager.acquire(node);
            if (s) {
                s.objects = {};
                s.objectScope = [];
                scopes.push(s);
                scopeStack.push(s);
            }
            if (node.type == "ObjectExpression") {
                if (isChild("init") && parentTypeIs("VariableDeclarator")) {
                    node.addedScope = true;
                    scopeStack[scopeStack.length - 1].objectScope.push(parents[parents.length - 2].id.name);
                } else if (isChild("value") && parentTypeIs("Property")) {
                    node.addedScope = true;
                    scopeStack[scopeStack.length - 1].objectScope.push(parents[parents.length - 2].key.name);
                }
            } else if (node.type == "MemberExpression") {
                node.name = resolveName(node);
                if (node.property.type == "Literal") {
                    // this one will not show up as an Identifier so we have to add it here
                    path = scopeStack[scopeStack.length - 1].objectScope.join(".");
                    if (path)
                        path += ".";
                    path += node.name;
                    add(path, node.property.range);
                }
                // log("got member expression", node.name, node.range);
            } else if (node.type == "Identifier") {
                path = scopeStack[scopeStack.length - 1].objectScope.join(".");
                if (path)
                    path += ".";
                var decl = false;
                if (parentTypeIs("MemberExpression") && isChild("property")) {
                    path += parents[parents.length - 2].name;
                } else {
                    if (parentTypeIs("Property") && parentTypeIs("ObjectExpression", parents.length - 2)
                        && isChild("init", parents.length - 2)) {
                        decl = true;
                    } else if (parentTypeIs("VariableDeclarator")) {
                        decl = true;
                    }
                    path += node.name;
                }
                add(path, node.range, decl); // probably more of them that should pass true
                // log("identifier", path, JSON.stringify(node.range));
            }
        },
        leave: function (node) {
            parents.pop();
            if (node.addedScope)
                scopeStack[scopeStack.length - 1].objectScope.pop();
            if (scopeManager.release(node)) // the scopeManager probably knows enough about this to provide the scopeStack
                scopeStack.pop();

        }
    });
    scopeManager.detach();

    var objects = [];
    for (var s=0; s<scopes.length; ++s) {
        objects.push(scopes[s].objects);
        log(s, scopes[s].objects);
    }
    cb(objects);
}

