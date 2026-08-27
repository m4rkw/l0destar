async function hashString(str) {
  const utf8 = new TextEncoder().encode(str);
  return await crypto.subtle.digest('SHA-256', utf8);
}

function stringToUint8Array(str) {
  const utf8 = unescape(encodeURIComponent(str));
  const arr = new Uint8Array(utf8.length);
  for (let i = 0; i < utf8.length; i++) {
    arr[i] = utf8.charCodeAt(i);
  }
  return arr;
}

function base64ToArrayBuffer(base64) {
  const binary = window.atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes.buffer;
}

function arrayBufferToBase64(buffer) {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return window.btoa(binary);
}

let options = null;

async function registerPasskey(username, token) {
  $('#message').text('registering passkey').show();

  // Build a fresh publicKey options object each call so retries work
  // (navigator.credentials.create consumes the BufferSource fields).
  const hashed = await hashString(options.user.id);
  const publicKey = Object.assign({}, options, {
    challenge: stringToUint8Array(options.challenge),
    user: Object.assign({}, options.user, { id: new Uint8Array(hashed) }),
  });

  if (Array.isArray(options.excludeCredentials) && options.excludeCredentials.length) {
    publicKey.excludeCredentials = options.excludeCredentials.map(c => Object.assign({}, c, {
      id: base64ToArrayBuffer(c.id.replace(/-/g, '+').replace(/_/g, '/')),
    }));
  }

  let credential;
  try {
    credential = await navigator.credentials.create({ publicKey: publicKey });
  } catch (e) {
    console.error('credentials.create failed:', e);
    $('#message').text('registration failed: ' + (e.name || 'error') + ' — ' + (e.message || '')).show();
    return;
  }

  const body = {
    id: credential.id,
    rawId: arrayBufferToBase64(credential.rawId),
    type: credential.type,
    response: {
      clientDataJSON: arrayBufferToBase64(credential.response.clientDataJSON),
      attestationObject: arrayBufferToBase64(credential.response.attestationObject),
    },
    user_id: username,
    token: token,
  };

  const resp = await fetch('/register?username=' + encodeURIComponent(username) + '&token=' + encodeURIComponent(token), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });

  if (resp.ok) {
    $('#message').text('passkey registered successfully').show();
    $('#continue').show();
  } else {
    const data = await resp.json().catch(() => ({}));
    $('#message').text(data.message || 'error registering passkey').show();
  }
}

$(document).ready(function() {
  const params = new URLSearchParams(window.location.search);
  const username = params.get('username');
  const token = params.get('token');

  $.ajax({
    type: 'POST',
    url: '/regoptions',
    data: 'username=' + encodeURIComponent(username) + '&token=' + encodeURIComponent(token),
    dataType: 'json',
    success: function(resp) {
      if (resp.status === 'ok') {
        options = resp.regoptions;
        $('input[name="register"]').removeAttr('disabled');
      } else {
        $('#message').text(resp.message || 'failed to load registration options').show();
      }
    },
    error: function(xhr) {
      const msg = (xhr.responseJSON && xhr.responseJSON.message) || 'failed to load registration options';
      $('#message').text(msg).show();
    },
  });

  $(document).on('click', 'input[name="register"]', function(e) {
    e.preventDefault();
    registerPasskey(username, token);
  });
});
