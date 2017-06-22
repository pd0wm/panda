/**
 * @file    panda.c
 * @author  Jeddy Diamond Exum
 * @date    16 June 2017
 * @version 0.1
 * @brief   Driver for the Comma.ai Panda CAN adapter to allow it to be controlled via
 * the Linux SocketCAN interface.
 * @see https://github.com/commaai/panda for the full project.
 */

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/init.h>             // Macros used to mark up functions e.g., __init __exit
#include <linux/kernel.h>           // Contains types, macros, functions for the kernel
#include <linux/module.h>           // Core header for loading LKMs into the kernel
#include <linux/netdevice.h>
#include <linux/usb.h>

/* vendor and product id */
#define PANDA_MODULE_NAME "panda"
#define PANDA_VENDOR_ID 0XBBAA
#define PANDA_PRODUCT_ID 0XDDCC

#define PANDA_MAX_TX_URBS 20
#define PANDA_CTX_FREE PANDA_MAX_TX_URBS

#define PANDA_USB_RX_BUFF_SIZE 0x40
#define PANDA_USB_TX_BUFF_SIZE (sizeof(struct panda_usb_can_msg))

#define PANDA_CAN_TRANSMIT 1
#define PANDA_CAN_EXTENDED 4

#define PANDA_BITRATE 500000

#define PANDA_DLC_MASK  0x0F

struct panda_usb_ctx {
  struct panda_priv *priv;
  u32 ndx;
  u8 dlc;
};

struct panda_priv {
  struct can_priv can;
  struct panda_usb_ctx tx_context[PANDA_MAX_TX_URBS];
  struct usb_device *udev;
  struct net_device *netdev;
  struct usb_anchor tx_submitted;
  struct usb_anchor rx_submitted;
  atomic_t free_ctx_cnt;
};

struct __packed panda_usb_can_msg {
  u32 rir;
  u32 bus_dat_len;
  u8 data[8];
};

static const struct usb_device_id panda_usb_table[] = {
  { USB_DEVICE(PANDA_VENDOR_ID, PANDA_PRODUCT_ID) },
  {} /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, panda_usb_table);

// CTX handling shamlessly ripped from mcba_usb.c linux driver
static inline void panda_init_ctx(struct panda_priv *priv)
{
  int i = 0;

  for (i = 0; i < PANDA_MAX_TX_URBS; i++) {
    priv->tx_context[i].ndx = PANDA_CTX_FREE;
    priv->tx_context[i].priv = priv;
  }

  atomic_set(&priv->free_ctx_cnt, ARRAY_SIZE(priv->tx_context));
}

static inline struct panda_usb_ctx *panda_usb_get_free_ctx(struct panda_priv *priv,
							 struct can_frame *cf)
{
  int i = 0;
  struct panda_usb_ctx *ctx = NULL;

  for (i = 0; i < PANDA_MAX_TX_URBS; i++) {
    if (priv->tx_context[i].ndx == PANDA_CTX_FREE) {
      ctx = &priv->tx_context[i];
      ctx->ndx = i;
      ctx->dlc = cf->can_dlc;

      atomic_dec(&priv->free_ctx_cnt);
      break;
    }
  }

  printk("CTX num %d\n", atomic_read(&priv->free_ctx_cnt));
  if (!atomic_read(&priv->free_ctx_cnt)){
    /* That was the last free ctx. Slow down tx path */
    printk("SENDING TOO FAST\n");
    netif_stop_queue(priv->netdev);
  }

  return ctx;
}

/* panda_usb_free_ctx and panda_usb_get_free_ctx are executed by different
 * threads. The order of execution in below function is important.
 */
static inline void panda_usb_free_ctx(struct panda_usb_ctx *ctx)
{
  /* Increase number of free ctxs before freeing ctx */
  atomic_inc(&ctx->priv->free_ctx_cnt);

  ctx->ndx = PANDA_CTX_FREE;

  /* Wake up the queue once ctx is marked free */
  netif_wake_queue(ctx->priv->netdev);
}



static void panda_urb_unlink(struct panda_priv *priv)
{
  usb_kill_anchored_urbs(&priv->rx_submitted);
  usb_kill_anchored_urbs(&priv->tx_submitted);
}

static int panda_set_output_enable(struct panda_priv* priv, bool enable){
  return usb_control_msg(priv->udev, usb_sndctrlpipe(priv->udev, 0),
			 0xDC, USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			 enable ? 0x1337 : 0, 0, NULL, 0, USB_CTRL_SET_TIMEOUT);
}

//static int panda_write_can(struct panda_priv* priv, u8 *buf, unsigned int len, int *actual_len){
//  return usb_bulk_msg(priv->udev, usb_sndbulkpipe(priv->udev, 3),
//		      buf, len, actual_len, 5000);
//}

static void panda_usb_write_bulk_callback(struct urb *urb)
{
  struct panda_usb_ctx *ctx = urb->context;
  struct net_device *netdev;

  WARN_ON(!ctx);

  netdev = ctx->priv->netdev;

  /* free up our allocated buffer */
  usb_free_coherent(urb->dev, urb->transfer_buffer_length,
		    urb->transfer_buffer, urb->transfer_dma);

  if (!netif_device_present(netdev))
    return;

  netdev->stats.tx_packets++;
  netdev->stats.tx_bytes += ctx->dlc;

  can_get_echo_skb(netdev, ctx->ndx);

  if (urb->status)
    netdev_info(netdev, "Tx URB aborted (%d)\n", urb->status);

  /* Release the context */
  panda_usb_free_ctx(ctx);
}


static netdev_tx_t panda_usb_xmit(struct panda_priv *priv,
				  struct panda_usb_can_msg *usb_msg,
				  struct panda_usb_ctx *ctx)
{
  struct urb *urb;
  u8 *buf;
  int err;

  /* create a URB, and a buffer for it, and copy the data to the URB */
  urb = usb_alloc_urb(0, GFP_ATOMIC);
  if (!urb)
    return -ENOMEM;

  buf = usb_alloc_coherent(priv->udev, PANDA_USB_TX_BUFF_SIZE, GFP_ATOMIC,
			   &urb->transfer_dma);
  if (!buf) {
    err = -ENOMEM;
    goto nomembuf;
  }

  memcpy(buf, usb_msg, PANDA_USB_TX_BUFF_SIZE);

  usb_fill_bulk_urb(urb, priv->udev,
		    usb_sndbulkpipe(priv->udev, 3), buf,
		    PANDA_USB_TX_BUFF_SIZE, panda_usb_write_bulk_callback,
		    ctx);

  urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
  usb_anchor_urb(urb, &priv->tx_submitted);

  err = usb_submit_urb(urb, GFP_ATOMIC);
  if (unlikely(err))
    goto failed;

  /* Release our reference to this URB, the USB core will eventually free it entirely. */
  usb_free_urb(urb);

  return 0;

 failed:
  usb_unanchor_urb(urb);
  usb_free_coherent(priv->udev, PANDA_USB_TX_BUFF_SIZE, buf, urb->transfer_dma);

  if (err == -ENODEV)
    netif_device_detach(priv->netdev);
  else
    netdev_warn(priv->netdev, "failed tx_urb %d\n", err);

 nomembuf:
  usb_free_urb(urb);

  return err;
}

static void panda_usb_process_can_rx(struct panda_priv *priv,
				     struct panda_usb_can_msg *msg)
{
  struct can_frame *cf;
  struct sk_buff *skb;
  struct net_device_stats *stats = &priv->netdev->stats;
  //u16 sid;

  skb = alloc_can_skb(priv->netdev, &cf);
  if (!skb)
    return;

  if(msg->rir & PANDA_CAN_EXTENDED){
    cf->can_id = (msg->rir >> 3) | CAN_EFF_FLAG;
  }else{
    cf->can_id = (msg->rir >> 21);
  }

  // TODO: Handle Remote Frames
  //if (msg->dlc & MCBA_DLC_RTR_MASK)
  //  cf->can_id |= CAN_RTR_FLAG;

  cf->can_dlc = get_can_dlc(msg->bus_dat_len & PANDA_DLC_MASK);

  memcpy(cf->data, msg->data, cf->can_dlc);

  stats->rx_packets++;
  stats->rx_bytes += cf->can_dlc;

  netif_rx(skb);
}

static void panda_usb_read_int_callback(struct urb *urb)
{
  struct panda_priv *priv = urb->context;
  struct net_device *netdev;
  int retval;
  int pos = 0;
  int num_recv = 0;

  netdev = priv->netdev;

  if (!netif_device_present(netdev))
    return;

  switch (urb->status) {
  case 0: /* success */
    break;
  case -ENOENT:
  case -ESHUTDOWN:
    return;
  default:
    netdev_info(netdev, "Rx URB aborted (%d)\n", urb->status);
    goto resubmit_urb;
  }

  while (pos < urb->actual_length) {
    struct panda_usb_can_msg *msg;

    if (pos + sizeof(struct panda_usb_can_msg) > urb->actual_length) {
      netdev_err(priv->netdev, "format error\n");
      break;
    }

    msg = (struct panda_usb_can_msg *)(urb->transfer_buffer + pos);

    num_recv++;
    panda_usb_process_can_rx(priv, msg);

    pos += sizeof(struct panda_usb_can_msg);
  }

 resubmit_urb:
  usb_fill_int_urb(urb, priv->udev,
		    usb_rcvintpipe(priv->udev, 1),
		    urb->transfer_buffer, PANDA_USB_RX_BUFF_SIZE,
		    panda_usb_read_int_callback, priv, 5);

  retval = usb_submit_urb(urb, GFP_ATOMIC);

  if (retval == -ENODEV)
    netif_device_detach(netdev);
  else if (retval)
    netdev_err(netdev, "failed resubmitting read bulk urb: %d\n", retval);
}


static int panda_usb_start(struct panda_priv *priv)
{
  struct net_device *netdev = priv->netdev;
  int err;
  struct urb *urb = NULL;
  u8 *buf;

  panda_init_ctx(priv);

  err = usb_set_interface(priv->udev, 0, 1);
  if (err) {
    netdev_err(netdev, "Can not set alternate setting to 1, error: %i", err);
    return err;
  }

  /* create a URB, and a buffer for it */
  urb = usb_alloc_urb(0, GFP_KERNEL);
  if (!urb) {
    return -ENOMEM;
  }

  buf = usb_alloc_coherent(priv->udev, PANDA_USB_RX_BUFF_SIZE,
			   GFP_KERNEL, &urb->transfer_dma);
  if (!buf) {
    netdev_err(netdev, "No memory left for USB buffer\n");
    usb_free_urb(urb);
    return -ENOMEM;
  }

  usb_fill_int_urb(urb, priv->udev,
                   usb_rcvintpipe(priv->udev, 1),
                   buf, PANDA_USB_RX_BUFF_SIZE,
                   panda_usb_read_int_callback, priv, 10);
  urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
  usb_anchor_urb(urb, &priv->rx_submitted);

  err = usb_submit_urb(urb, GFP_KERNEL);
  if (err) {
  usb_unanchor_urb(urb);
    usb_free_coherent(priv->udev, PANDA_USB_RX_BUFF_SIZE, buf, urb->transfer_dma);
    usb_free_urb(urb);
    netdev_err(netdev, "Failed in start, while submitting urb.\n");
    return err;
  }

  /* Drop reference, USB core will take care of freeing it */
  usb_free_urb(urb);


  return 0;
}

/* Open USB device */
static int panda_usb_open(struct net_device *netdev)
{
  struct panda_priv *priv = netdev_priv(netdev);
  int err;

  /* common open */
  err = open_candev(netdev);
  if (err)
    return err;

  //priv->can_speed_check = true;
  priv->can.state = CAN_STATE_ERROR_ACTIVE;

  netif_start_queue(netdev);

  return 0;
}

/* Close USB device */
static int panda_usb_close(struct net_device *netdev)
{
  struct panda_priv *priv = netdev_priv(netdev);

  priv->can.state = CAN_STATE_STOPPED;

  netif_stop_queue(netdev);

  /* Stop polling */
  panda_urb_unlink(priv);

  close_candev(netdev);

  return 0;
}

static netdev_tx_t panda_usb_start_xmit(struct sk_buff *skb,
					struct net_device *netdev)
{
  struct panda_priv *priv = netdev_priv(netdev);
  struct can_frame *cf = (struct can_frame *)skb->data;
  struct panda_usb_ctx *ctx = NULL;
  struct net_device_stats *stats = &priv->netdev->stats;
  int err;
  struct panda_usb_can_msg usb_msg = {};
  int bus = 0;

  if (can_dropped_invalid_skb(netdev, skb)){
    printk("Invalid CAN packet");
    return NETDEV_TX_OK;
  }

  ctx = panda_usb_get_free_ctx(priv, cf);

  //Warning: cargo cult. Can't tell what this is for, but it is
  //everywhere and encouraged in the documentation.
  can_put_echo_skb(skb, priv->netdev, ctx->ndx);

  if(cf->can_id & CAN_EFF_FLAG){
    usb_msg.rir = cpu_to_le32(((cf->can_id & 0x1FFFFFFF) << 3) |
			      PANDA_CAN_TRANSMIT | PANDA_CAN_EXTENDED);
  }else{
    usb_msg.rir = cpu_to_le32(((cf->can_id & 0x7FF) << 21) | PANDA_CAN_TRANSMIT);
  }
  usb_msg.bus_dat_len = cpu_to_le32((cf->can_dlc & 0x0F) | (bus << 4));

  memcpy(usb_msg.data, cf->data, cf->can_dlc);

  //TODO Handle Remote Frames
  //if (cf->can_id & CAN_RTR_FLAG)
  //  usb_msg.dlc |= PANDA_DLC_RTR_MASK;

  netdev_err(netdev, "Received data from socket. canid: %x; len: %d\n", cf->can_id, cf->can_dlc);

  err = panda_usb_xmit(priv, &usb_msg, ctx);
  if (err)
    goto xmit_failed;

  return NETDEV_TX_OK;

 xmit_failed:
  can_free_echo_skb(priv->netdev, ctx->ndx);
  panda_usb_free_ctx(ctx);
  dev_kfree_skb(skb);
  stats->tx_dropped++;

  return NETDEV_TX_OK;
}

static const struct net_device_ops panda_netdev_ops = {
  .ndo_open = panda_usb_open,
  .ndo_stop = panda_usb_close,
  .ndo_start_xmit = panda_usb_start_xmit,
};

static int panda_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
  struct net_device *netdev;
  struct panda_priv *priv;
  int err = -ENOMEM;
  struct usb_device *usbdev = interface_to_usbdev(intf);

  netdev = alloc_candev(sizeof(struct panda_priv), PANDA_MAX_TX_URBS);
  if (!netdev) {
    dev_err(&intf->dev, "Couldn't alloc candev\n");
    return -ENOMEM;
  }

  priv = netdev_priv(netdev);

  priv->udev = usbdev;
  priv->netdev = netdev;

  init_usb_anchor(&priv->rx_submitted);
  init_usb_anchor(&priv->tx_submitted);

  usb_set_intfdata(intf, priv);

  /* Init CAN device */
  priv->can.state = CAN_STATE_STOPPED;
  //priv->can.do_set_termination = panda_set_termination;
  //priv->can.do_set_mode = panda_net_set_mode;
  //priv->can.do_get_berr_counter = panda_net_get_berr_counter;
  //priv->can.do_set_bittiming = panda_net_set_bittiming;

  priv->can.bittiming.bitrate = PANDA_BITRATE;

  netdev->netdev_ops = &panda_netdev_ops;

  netdev->flags |= IFF_ECHO; /* we support local echo */

  SET_NETDEV_DEV(netdev, &intf->dev);

  err = register_candev(netdev);
  if (err) {
    netdev_err(netdev, "couldn't register PANDA CAN device: %d\n", err);
    goto cleanup_free_candev;
  }

  err = panda_usb_start(priv);
  if (err) {
    dev_err(&intf->dev, "Failed to initialize Comma.ai Panda CAN controller\n");
    goto cleanup_unregister_candev;
  }

  err = panda_set_output_enable(priv, true);
  if (err) {
    dev_info(&intf->dev, "Failed to initialize send enable message to Panda.\n");
    goto cleanup_unregister_candev;
  }

  dev_info(&intf->dev, "Comma.ai Panda CAN controller connected\n");

  return 0;

 cleanup_unregister_candev:
  unregister_candev(priv->netdev);

 cleanup_free_candev:
  free_candev(priv->netdev);

  return err;
}

/* Called by the usb core when driver is unloaded or device is removed */
static void panda_usb_disconnect(struct usb_interface *intf)
{
  struct panda_priv *priv = usb_get_intfdata(intf);

  usb_set_intfdata(intf, NULL);

  netdev_info(priv->netdev, "device disconnected\n");

  unregister_candev(priv->netdev);
  free_candev(priv->netdev);

  panda_urb_unlink(priv);
}

static struct usb_driver panda_usb_driver = {
  .name = PANDA_MODULE_NAME,
  .probe = panda_usb_probe,
  .disconnect = panda_usb_disconnect,
  .id_table = panda_usb_table,
};

module_usb_driver(panda_usb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jessy Diamond Exum <jessy.diamondman@gmail.com>");
MODULE_DESCRIPTION("SocketCAN driver for Comma.ai's Panda Adapter.");
MODULE_VERSION("0.1");
