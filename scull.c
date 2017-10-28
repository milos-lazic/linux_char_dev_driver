#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Milos");
MODULE_DESCRIPTION("scull - a character device driver");
MODULE_VERSION("0.1");

#define DEVICE_NAME "scull"
#define NUM_SCULL_DEVICES 2




/* Scull file operations prototype */
static int scull_open( struct inode *, struct file *);
static int scull_release( struct inode *, struct file *);
static ssize_t scull_read( struct file *, char *, size_t, loff_t *);
static ssize_t scull_write( struct file *, const char *, size_t, loff_t *);
static loff_t scull_llseek( struct file *, loff_t, int);



#define SCULL_PAGE_SIZE      4096
#define SCULL_QSET_LEN       32
#define SCULL_QUANTUM_LEN    (SCULL_PAGE_SIZE/SCULL_QSET_LEN)

struct scull_page {
	char              **qsetpp;
	struct scull_page  *next;
};



struct scull_dev {
	struct cdev             cdev;             /* cdev structure */
	struct scull_page      *datap;            /* pointer to data */
	loff_t                  size;             /* file size  - SCULL_TBD: not yet in use */
	char                    name[10];         /* device name */
	struct mutex            mx;               /* mutex */
};


static struct file_operations scull_fops = {

	.owner = THIS_MODULE,
	.open = scull_open,
	.read = scull_read,
	.write = scull_write,
	.release = scull_release,
	.llseek = scull_llseek,
};



static dev_t               scull_dev_number;
static struct scull_dev   *scull_devp[NUM_SCULL_DEVICES];
struct class              *scull_class;



static int __init scull_init(void)
{
	int rv, i;

	printk(KERN_INFO "scull: start driver initialization\n");

	rv = alloc_chrdev_region( &scull_dev_number, 0, NUM_SCULL_DEVICES, DEVICE_NAME);
	if ( rv < 0)
	{
		printk(KERN_ALERT "scull: unable to register device\n");
		return -1;
	}

	printk(KERN_INFO "scull: device registered; major: %d\n", MAJOR(scull_dev_number));

	scull_class = class_create( THIS_MODULE, DEVICE_NAME);

	for ( i = 0; i < NUM_SCULL_DEVICES; i++)
	{
		/* Allocate memory for per-device structures */
		scull_devp[i] = kmalloc( sizeof(struct scull_dev), GFP_KERNEL);
		if ( scull_devp[i] == NULL)
		{
			printk(KERN_ALERT "scull: kmalloc error\n");
			return -ENOMEM;
		}

		/* initialize device mutex */
		mutex_init( &scull_devp[i]->mx);

		/* store device name */
		sprintf( scull_devp[i]->name, "scull%d", i);

		/* set data pointer to null (no data stored on initializtaion) */
		scull_devp[i]->datap = NULL;

		/* Connect file operations with cdev */
		cdev_init( &scull_devp[i]->cdev, &scull_fops);
		scull_devp[i]->cdev.owner = THIS_MODULE;

		/* Connect major/minor number to cdev and add to system*/
		rv = cdev_add( &scull_devp[i]->cdev, (scull_dev_number + i), 1);
		if ( rv < 0)
		{
			printk( KERN_ALERT "scull: cdev add failed for scull%d\n", i);
			return rv;
		}

		/* Send uevents to udev to generate /dev nodes */
		device_create(scull_class, NULL, MKDEV(MAJOR(scull_dev_number), i), NULL, "scull%d", i);
	}

	printk(KERN_INFO "scull: driver initialized\n");

	return 0;
}



static void __exit scull_exit(void)
{
	int i, j;
	struct scull_page *currpagep, *nextpagep;

	printk(KERN_INFO "scull: exit\n");


	for ( i = 0; i < NUM_SCULL_DEVICES; i++)
	{
		/* free all previously allocated objects */
		currpagep = scull_devp[i]->datap;

		while (currpagep != NULL)
		{
			nextpagep = currpagep->next;

			for( j = 0; j < SCULL_QSET_LEN; j++)
			{
				kfree( currpagep->qsetpp[j]);
			}

			kfree( currpagep->qsetpp);

			kfree( currpagep);

			currpagep = nextpagep;
		}
		

		device_destroy(scull_class, MKDEV(MAJOR(scull_dev_number), i));
		cdev_del(&scull_devp[i]->cdev);
		kfree(scull_devp[i]);	
	}

	class_destroy(scull_class);

	unregister_chrdev_region( scull_dev_number, NUM_SCULL_DEVICES);

	return;
}



static int scull_open( struct inode *inodep, struct file *filep)
{
	struct scull_dev *scull_devp;

	/* Get per-device structure that stores the corresponding cdev */
	scull_devp = container_of(inodep->i_cdev, struct scull_dev, cdev);

	/* Easy access to scull_devp from rest of entry points */
	filep->private_data = scull_devp;

	printk(KERN_INFO "%s: called scull_open()\n", scull_devp->name);

	return 0;
}



static int scull_release( struct inode *inodep, struct file *filep)
{
	struct scull_dev *scull_devp = filep->private_data;

	printk(KERN_INFO "%s: device closed\n", scull_devp->name);
	return 0;
}



static ssize_t scull_read( struct file *filep, char __user *buf, size_t count, loff_t *ppos)
{
	struct scull_dev *scull_devp = filep->private_data;
	unsigned int pagenum = *ppos / SCULL_PAGE_SIZE;
	unsigned int qsetnum = (*ppos % SCULL_PAGE_SIZE) / SCULL_QUANTUM_LEN;
	unsigned int qidx    = (*ppos % SCULL_PAGE_SIZE) % SCULL_QUANTUM_LEN;
	unsigned int i, rem, bytes_read;
	struct scull_page *currpagep;
	char *charp;

	printk(KERN_INFO "%s: called scull_read()\n", scull_devp->name);

	currpagep = scull_devp->datap; /* set current page pointer to page 0 */

	/* enter critical section */
	mutex_lock( &scull_devp->mx); /* SCULL_TBD: replace with a read lock */

	i = 0;
	while ( i < pagenum && currpagep != NULL)
	{
		currpagep = currpagep->next;
	}

	if ( currpagep == NULL)
	{
		/* leave critical section */
		mutex_unlock( &scull_devp->mx);
		return 0;
	}

	/* rem - remaining unwritten data */
	rem = ( (SCULL_QUANTUM_LEN - qidx) < count) ? (SCULL_QUANTUM_LEN-qidx) : count;
	charp = &currpagep->qsetpp[qsetnum][qidx];
	bytes_read = 0;

	while( rem && *charp != '\0')
	{
		copy_to_user( buf++, charp++, 1);

		rem --;
		bytes_read++;
	}

	/* update file offset counter */
	*ppos += bytes_read;

	/* leave critical section */
	mutex_unlock( &scull_devp->mx);

	return bytes_read;
}


static ssize_t scull_write( struct file *filep, const char *buf, size_t count, loff_t *ppos)
{
	struct scull_dev *scull_devp = filep->private_data;
	unsigned int pagenum = *ppos / SCULL_PAGE_SIZE;
	unsigned int qsetnum = (*ppos % SCULL_PAGE_SIZE) / SCULL_QUANTUM_LEN;
	unsigned int qidx    = (*ppos % SCULL_PAGE_SIZE) % SCULL_QUANTUM_LEN;
	unsigned int i, j, rem;
	struct scull_page *currpagep, *prevpagep;

	printk(KERN_INFO "%s: called scull_write()\n", scull_devp->name);

	/* enter critical section */
	mutex_lock( &scull_devp->mx);

	/* Create first page node if this is the first call to the .write method */
	if ( scull_devp->datap == NULL)
	{
		scull_devp->datap = (struct scull_page *) kmalloc ( sizeof(struct scull_page), GFP_KERNEL);
		if ( scull_devp->datap == NULL)
		{
			/* Unable to allocate a new scull_page struct */
			printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

			/* leave critical section */
			mutex_unlock( &scull_devp->mx);

			return -ENOMEM;
		}

		scull_devp->datap->next = NULL;

		/* Allocate a quantum set (array of pointers-to-char) */
		scull_devp->datap->qsetpp = (char **) kmalloc( SCULL_QSET_LEN * sizeof(char *), GFP_KERNEL);
		if ( scull_devp->datap->qsetpp == NULL)
		{
			/* Unable to allocate a new quantum set; undo previous kmalloc() */
			printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

			kfree(scull_devp->datap);

			/* leave critical section */
			mutex_unlock( &scull_devp->mx);

			return -ENOMEM;
		}

		/* Allocate quanta */
		for ( i = 0; i < SCULL_QSET_LEN; i++)
		{
			scull_devp->datap->qsetpp[i] = (char *) kmalloc( SCULL_QUANTUM_LEN * sizeof(char), GFP_KERNEL);
			if ( scull_devp->datap->qsetpp[i] == NULL)
			{
				/* Unable to allocate new quantum; undo previous kmallocs */
				printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

				for( i = i -1; i >= 0; i--)
				{
					kfree(scull_devp->datap->qsetpp[i]);
				}

				kfree( scull_devp->datap->qsetpp);

				kfree( scull_devp->datap);

				/* leave critical section */
				mutex_unlock( &scull_devp->mx);

				return -ENOMEM;
			}
		}
	}

	currpagep = scull_devp->datap; /* point to page 0 */
	prevpagep = NULL;

	/* NOTE: Algorithm for traversing pages (some may not be allocated)
	 *
	 * i = 0;
	 * while ( i < pagenum)
	 * {
	 *    if ( currpagep == NULL)
	 *       create_new_page()
	 *       i++
	 *    else
	 *       prevpagep = currpagep;
	 *       currpagep = currpagep->next;
	 *       if ( currpagep == NULL)
	 *          // do not increment i
	 *       else
	 *          i++
	 * }
	 *
	 */

	i = 0;
	while ( i < pagenum)
	{
		if ( currpagep == NULL)
		{
			/* Create new page */
			currpagep = (struct scull_page *) kmalloc( sizeof( struct scull_page), GFP_KERNEL);
			if ( currpagep == NULL)
			{
				/* Unable to create new page */
				printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

				/* leave critical section */
				mutex_unlock( &scull_devp->mx);

				return -ENOMEM;
			}

			currpagep->next = NULL;

			/* Allocate quantum set */
			currpagep->qsetpp = (char **) kmalloc ( SCULL_QSET_LEN * sizeof( char *), GFP_KERNEL);
			if ( currpagep->qsetpp == NULL)
			{
				/* Failed to create quantum set; undo previous kmalloc */
				printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

				kfree( currpagep);

				/* leave critical section */
				mutex_unlock( &scull_devp->mx);

				return -ENOMEM;
			}

			/* Allocate quanta */
			for ( j = 0; j < SCULL_QSET_LEN; j++)
			{
				currpagep->qsetpp[j] = (char *) kmalloc ( SCULL_QUANTUM_LEN * sizeof(char), GFP_KERNEL);
				if ( currpagep->qsetpp[j] == NULL)
				{
					/* Faile to create quantum; undor previous kmallocs */
					printk(KERN_ALERT "%s: bad kmalloc (line %u)\n", scull_devp->name, __LINE__);

					for ( j = j - 1; j >= 0; j--)
					{
						kfree( currpagep->qsetpp[j]);
					}

					kfree( currpagep->qsetpp);

					kfree( currpagep);

					/* leave critical section */
					mutex_unlock( &scull_devp->mx);

					return -ENOMEM;
				}
			}

			prevpagep->next = currpagep;

			i++;
		}
		else
		{
			prevpagep = currpagep;
			currpagep = currpagep->next;

			if ( currpagep == NULL)
			{
				// do not increment i
			}
			else
			{
				i++;
			}
		}
	}

	/* SCULL_TBD: rework section below */

	/* Insert data from user (copy_from_user) */
	rem = SCULL_QUANTUM_LEN - qidx; /* space remaining in current quantum */

	if( rem < count)
	{
		copy_from_user( &currpagep->qsetpp[qsetnum][qidx], buf, rem);
		*ppos += rem;

		/* leave critical section */
		mutex_unlock( &scull_devp->mx);

		return rem;
	}
	else
	{
		copy_from_user( &currpagep->qsetpp[qsetnum][qidx], buf, count);
		*ppos += count;

		/* leave critical section */
		mutex_unlock( &scull_devp->mx);

		return count;
	}

	/* leave critical section */
	mutex_unlock( &scull_devp->mx);

	/* code should never reach this statement.. */
	return count;
}



static loff_t scull_llseek( struct file *filep, loff_t off, int whence)
{
	struct scull_dev *scull_devp = filep->private_data;
	int newpos;

	switch( whence)
	{
		case 0: /* SEEK_SET */
			newpos = off;
			break;

		case 1: /* SEEK_CUR */
			newpos = filep->f_pos + off;
			break;

		case 2: /* SEEK_END */
			return -EINVAL; /* SCULL_TBD: not yet implemented */

		default: /* Can't happen */
			return -EINVAL;
	}

	/* check that newpos is valid */
	if ( newpos < 0)
		return -EINVAL;


	/* enter critical section */
	mutex_lock( &scull_devp->mx);

	/* update file offset */
	filep->f_pos = newpos;

	/* leave critical section */
	mutex_unlock( &scull_devp->mx);

	return filep->f_pos;
}



module_init(scull_init);
module_exit(scull_exit);

